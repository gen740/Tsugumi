// Remote reception and detached supervision share one binary. Only --invoke
// initialises Nix; listeners and carriers operate on bounded protocol frames.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "journal.hh"
#include "tsugumi/workerd/log.hh"
#include "tsugumi/workerd/log_limits.hh"
#include "util/crypto.hh"
#include "util/fs.hh"
#include "util/peer_tls.hh"

extern char** environ;
namespace tsugumi::workerd {
auto invoke(const stdfs::path& directory, const stdfs::path& blobs,
            const stdfs::path& store, uint64_t log_limit_bytes) -> void;
namespace {
using namespace std::chrono_literals;
constexpr size_t kConnections = 64;
volatile sig_atomic_t stopping = 0;
auto stop(int) -> void { stopping = 1; }
struct Fd {
  int value = -1;
  Fd() = default;
  explicit Fd(int fd) : value(fd) {}
  Fd(const Fd&) = delete;
  auto operator=(const Fd&) -> Fd& = delete;
  ~Fd() {
    if (value >= 0) ::close(value);
  }
};
auto log(const std::string& line) -> void {
  std::cerr << "tsugumi-remote-worker: " << line << '\n';
}
auto boot() -> std::string {
  return fs::read_file_or_throw("/proc/sys/kernel/random/boot_id");
}
auto process_start(pid_t pid) -> uint64_t {
  const auto raw =
      fs::read_file_or_throw("/proc/" + std::to_string(pid) + "/stat");
  const auto close = raw.rfind(')');
  if (close == std::string::npos)
    throw std::runtime_error("invalid process identity");
  std::istringstream fields(raw.substr(close + 2));
  std::string field;
  for (unsigned i = 0; i < 20; ++i)
    if (!(fields >> field))
      throw std::runtime_error("invalid process identity");
  return std::stoull(field);
}
auto verified_kill(const Record& state) -> void {
  if (state.pid <= 1 || state.boot != boot()) return;
  Fd pidfd(static_cast<int>(::syscall(SYS_pidfd_open, state.pid, 0)));
  if (pidfd.value < 0) return;
  try {
    if (process_start(static_cast<pid_t>(state.pid)) == state.start)
      ::syscall(SYS_pidfd_send_signal, pidfd.value, SIGKILL, nullptr, 0);
  } catch (const std::exception&) { /* The recorded process already exited. */
  }
}
auto cgroup_write(const stdfs::path& path, const std::string& value) -> void {
  std::ofstream file(path);
  file << value;
  file.close();
  if (!file) throw std::runtime_error("cannot configure delegated job cgroup");
}
auto check_cgroups(const stdfs::path& root) -> void {
  if (root.empty()) return;
  struct statfs s{};
  struct stat st{};
  if (!root.is_absolute() || ::statfs(root.c_str(), &s) != 0 ||
      s.f_type != 0x63677270 || ::stat(root.c_str(), &st) != 0 ||
      st.st_uid != ::geteuid())
    throw std::runtime_error(
        "--cgroup-root must be an owned delegated cgroup v2 subtree");
  cgroup_write(root / "cgroup.subtree_control", "+cpu +memory");
  const auto enabled = fs::read_file_or_throw(root / "cgroup.subtree_control");
  if (enabled.find("cpu") == std::string::npos ||
      enabled.find("memory") == std::string::npos)
    throw std::runtime_error(
        "cgroup CPU and memory controllers were not enabled");
}
auto cgroup_path(const stdfs::path& root, const Journal& journal,
                 const std::string& job) -> stdfs::path {
  if (root.empty()) return {};
  return root /
         ("tsugumi-" +
          crypto::sha256_hex(journal.root().string() + job).substr(0, 32));
}
auto remove_cgroup(const stdfs::path& path) -> void {
  if (path.empty() || !stdfs::exists(path)) return;
  try {
    cgroup_write(path / "cgroup.kill", "1");
  } catch (const std::exception&) {
  }
  std::error_code ec;
  stdfs::remove(path, ec);
}
auto make_cgroup(const stdfs::path& path, const R::JobSpecT& spec) -> void {
  remove_cgroup(path);
  if (!stdfs::create_directory(path))
    throw std::runtime_error("cannot create private job cgroup");
  const auto quota = std::to_string(spec.cpu_millis * 1000) + " 1000000";
  cgroup_write(path / "cpu.max", quota);
  cgroup_write(path / "memory.max", std::to_string(spec.memory_bytes));
  auto cpu = fs::read_file_or_throw(path / "cpu.max");
  auto memory = fs::read_file_or_throw(path / "memory.max");
  if (cpu != quota + "\n" ||
      std::stoull(memory) !=
          spec.memory_bytes / static_cast<uint64_t>(::sysconf(_SC_PAGESIZE)) *
              static_cast<uint64_t>(::sysconf(_SC_PAGESIZE)))
    throw std::runtime_error("kernel did not apply the requested job limits");
}
auto supervisor_lock(const Journal& journal, const std::string& job) -> int {
  check_id(job);
  return ::open((journal.root() / "locks" / (job + ".supervisor")).c_str(),
                O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
}

struct Config {
  stdfs::path root;
  stdfs::path unix_socket;
  stdfs::path authorized_keys;
  bool print_key = false;
  stdfs::path cgroup;
  std::string host = "127.0.0.1";
  uint16_t port = 8765;
  uint32_t max_jobs = 8;
  uint64_t log_limit_bytes = kDefaultLogLimitBytes;
  std::string executable;
};
auto default_root() -> stdfs::path {
  if (const char* value = std::getenv("TSUGUMI_REMOTE_STATE_DIR")) return value;
  if (const char* value = std::getenv("XDG_STATE_HOME");
      value && stdfs::path(value).is_absolute())
    return stdfs::path(value) / "tsugumi/remote-execution";
  const char* home = std::getenv("HOME");
  if (!home) throw std::runtime_error("HOME is not set");
  return stdfs::path(home) / ".local/state/tsugumi/remote-execution";
}
auto check_namespace(const std::string& self) -> void {
  std::vector<std::string> args{"unshare",
                                "--map-current-user",
                                "--pid",
                                "--fork",
                                "--kill-child=SIGKILL",
                                "--mount-proc",
                                "--",
                                self,
                                "--check-namespace"};
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  pid_t pid = -1;
  const auto rc =
      ::posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0)
    throw std::runtime_error("cannot start the unshare namespace launcher");
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  int status = 0;
  for (;;) {
    const auto done = ::waitpid(pid, &status, WNOHANG);
    if (done == pid) break;
    if (done < 0 && errno != EINTR)
      throw std::runtime_error("cannot wait for namespace check");
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(pid, SIGKILL);
      while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
      }
      throw std::runtime_error("namespace startup check timed out");
    }
    std::this_thread::sleep_for(10ms);
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw std::runtime_error(
        "remote worker requires unprivileged user, mount and PID namespaces");
}
auto system() -> std::string {
  utsname u{};
  if (::uname(&u) != 0)
    throw std::runtime_error("cannot identify worker system");
  const std::string arch(u.machine);
  if (arch != "x86_64" && arch != "aarch64")
    throw std::runtime_error("unsupported Linux worker system");
  return arch + "-linux";
}

class Daemon {
 public:
  explicit Daemon(Config config)
      : config_(std::move(config)),
        server_(identity(config_.root, "server_id")),
        system_(system()) {
    private_directory(config_.root / "projects");
    private_directory(config_.root / "store");
    check_cgroups(config_.cgroup);
    check_namespace(config_.executable);
  }
  auto capabilities(const std::string& instance = {}) const
      -> R::CapabilitiesT {
    R::CapabilitiesT c;
    c.server_id = server_;
    c.instance_id = instance;
    c.native_system = system_;
    c.systems = {system_};
    c.resource_limits = !config_.cgroup.empty();
    c.max_jobs = config_.max_jobs;
    c.worker_contract = kContract;
    return c;
  }
  auto project(const std::string& scope) -> std::shared_ptr<Journal> {
    check_id(scope);
    std::lock_guard guard(project_mutex_);
    auto& entry = projects_[scope];
    if (!entry)
      entry = std::make_shared<Journal>(config_.root / "projects" / scope);
    return entry;
  }
  auto handle(const R::RequestT& q) -> R::MessageT {
    if (!q.server_id.empty() && q.server_id != server_)
      throw Error(R::ErrorCode::InstanceChanged,
                  "endpoint now serves a different worker");
    if (q.body.type == R::RequestBody::NONE ||
        q.body.type == R::RequestBody::CatalogRequest)
      throw Error(R::ErrorCode::Invalid, "invalid remote request");
    if (q.project_scope.empty()) {
      if (q.body.type != R::RequestBody::ProbeRequest || !q.instance_id.empty())
        throw Error(R::ErrorCode::Rejected,
                    "host-authorized project scope required");
      return reply(capabilities());
    }
    if (q.server_id.empty())
      throw Error(R::ErrorCode::Rejected, "pinned server identity required");
    const auto journal = project(q.project_scope);
    if ((!q.instance_id.empty() && q.instance_id != journal->instance()) ||
        (q.body.type != R::RequestBody::ProbeRequest && q.instance_id.empty()))
      throw Error(R::ErrorCode::InstanceChanged,
                  "project journal identity changed");
    if (q.body.type == R::RequestBody::ProbeRequest)
      return reply(capabilities(journal->instance()));
    check_id(q.job_id);
    switch (q.body.type) {
      case R::RequestBody::PutBlobRequest:
        journal->put(q.job_id, *q.body.AsPutBlobRequest());
        return reply(R::AckT{});
      case R::RequestBody::GetBlobRequest:
        return reply(journal->get(q.job_id, *q.body.AsGetBlobRequest()));
      case R::RequestBody::SubmitRequest: {
        const auto& bytes = q.body.AsSubmitRequest()->spec;
        const std::string_view data(reinterpret_cast<const char*>(bytes.data()),
                                    bytes.size());
        const auto spec = parse_spec(data);
        if (spec->system != system_)
          throw Error(R::ErrorCode::Rejected,
                      "job system is not supported by this worker");
        if (spec->cpu_millis &&
            (config_.cgroup.empty() ||
             spec->memory_bytes <
                 static_cast<uint64_t>(::sysconf(_SC_PAGESIZE))))
          throw Error(R::ErrorCode::Rejected,
                      "hard limits require a delegated cgroup v2 subtree and "
                      "at least one page of memory");
        return reply(journal->submit(q.job_id, data).status());
      }
      case R::RequestBody::StatusRequest:
        return reply(journal->status(q.job_id).status());
      case R::RequestBody::CancelRequest:
        return reply(
            journal->cancel(q.job_id, q.body.AsCancelRequest()->fingerprint)
                .status());
      case R::RequestBody::ReleaseRequest:
        journal->release(q.job_id, q.body.AsReleaseRequest()->fingerprint);
        return reply(R::AckT{});
      default:
        throw Error(R::ErrorCode::Invalid, "invalid request body");
    }
  }
  // Any secure carrier can call this framed stream handler. Authentication is
  // deliberately separate from framing and dispatch.
  auto serve_stream(peer_tls::Stream& stream) -> void {
    try {
      auto n = frame_length(stream.Read(4));
      auto m = decode(stream.Read(n));
      auto* q = m->payload.AsRequest();
      if (!q) throw Error(R::ErrorCode::Invalid, "expected a request");
      auto response = encode(handle(*q));
      stream.Write(length_prefix(static_cast<uint32_t>(response.size())));
      stream.Write(response);
    } catch (const Error& e) {
      auto bytes = encode(failure(e.code, e.what()));
      stream.Write(length_prefix(static_cast<uint32_t>(bytes.size())) + bytes);
    }
  }
  auto tick() -> void {
    while (::waitpid(-1, nullptr, WNOHANG) > 0) {
    }
    for (const auto& entry :
         stdfs::directory_iterator(config_.root / "projects"))
      if (crypto::is_sha256_hex(entry.path().filename().string()))
        project(entry.path().filename().string());
    std::vector<std::pair<std::shared_ptr<Journal>, std::string>> pending;
    size_t active = 0;
    std::vector<std::shared_ptr<Journal>> projects;
    {
      std::lock_guard guard(project_mutex_);
      for (const auto& [_, p] : projects_) projects.push_back(p);
    }
    for (const auto& p : projects)
      for (const auto& job : p->jobs()) {
        try {
          Fd lock(supervisor_lock(*p, job));
          if (lock.value < 0)
            throw std::runtime_error("cannot open supervisor lock");
          auto state = p->status(job);
          if (::flock(lock.value, LOCK_EX | LOCK_NB) != 0) {
            ++active;
            const auto key = p->root().string() + job;
            if (state.state == R::JobState::Cancelling &&
                !cancelling_.contains(key))
              cancelling_[key] = now_ms();
            if ((state.deadline && now_ms() > state.deadline + 15000) ||
                (cancelling_.contains(key) &&
                 now_ms() > cancelling_[key] + 15000))
              verified_kill(state);
            continue;
          }
          cancelling_.erase(p->root().string() + job);
          if (state.state == R::JobState::Running ||
              state.state == R::JobState::Cancelling) {
            remove_cgroup(cgroup_path(config_.cgroup, *p, job));
            discard_tree(p->job_path(job) / "work");
            p->finish(job, R::JobState::Interrupted, {},
                      "execution supervisor ended without recording a result");
          }
          if (terminal(state.state)) {
            remove_cgroup(cgroup_path(config_.cgroup, *p, job));
            discard_tree(p->job_path(job) / "work");
          }
          p->expire(job);
          if (p->status(job).state == R::JobState::Queued)
            pending.emplace_back(p, job);
        } catch (const std::exception& e) {
          ++active;
          log(e.what());
        }
      }
    std::ranges::sort(pending, [](const auto& a, const auto& b) {
      return stdfs::last_write_time(a.first->job_path(a.second) / "spec.fb") <
             stdfs::last_write_time(b.first->job_path(b.second) / "spec.fb");
    });
    for (const auto& [p, job] : pending) {
      if (active >= config_.max_jobs || stopping) break;
      Fd lock(supervisor_lock(*p, job));
      if (lock.value < 0 || ::flock(lock.value, LOCK_EX | LOCK_NB) != 0)
        continue;
      // Commit running before spawn: a crash in this gap becomes interrupted,
      // never an automatic second execution of the same invocation.
      if (!p->claim(job, 0, 0, {})) continue;
      if (spawn(*p, job, lock.value))
        ++active;
      else
        p->finish(job, R::JobState::Failed, {}, "supervisor launch failed");
    }
  }

 private:
  auto spawn(Journal& p, const std::string& job, int lock) -> bool {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, lock, 5);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(
        &actions, 1, (p.job_path(job) / "supervisor.log").c_str(),
        O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    std::vector<std::string> args{config_.executable,
                                  "--supervise",
                                  p.root().string(),
                                  job,
                                  "5",
                                  (config_.root / "store").string(),
                                  config_.cgroup.string(),
                                  std::to_string(config_.log_limit_bytes)};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc =
        ::posix_spawn(&pid, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc == 0) {
      try {
        p.supervisor_identity(job, static_cast<uint64_t>(pid),
                              process_start(pid), boot());
      } catch (const std::exception&) { /* The supervisor may already have
                                           completed. */
      }
    }
    return rc == 0;
  }
  Config config_;
  std::string server_, system_;
  std::mutex project_mutex_;
  std::map<std::string, std::shared_ptr<Journal>> projects_;
  std::map<std::string, uint64_t> cancelling_;
};

auto supervise(const std::string& self, const stdfs::path& root,
               const std::string& job, int lockfd, const stdfs::path& store,
               const stdfs::path& cgroup_root, uint64_t log_limit_bytes) -> int {
  Journal journal(root);
  Fd expected(supervisor_lock(journal, job));
  struct stat held{}, named{};
  if (expected.value < 0 || ::fstat(lockfd, &held) != 0 ||
      ::fstat(expected.value, &named) != 0 || held.st_dev != named.st_dev ||
      held.st_ino != named.st_ino || ::flock(lockfd, LOCK_EX | LOCK_NB) != 0)
    throw std::runtime_error("supervisor requires its inherited job lock");
  ::fcntl(lockfd, F_SETFD, FD_CLOEXEC);
  auto state = journal.status(job);
  if (state.state != R::JobState::Running &&
      state.state != R::JobState::Cancelling)
    return 0;
  // Record the identity before launching a sandbox, for the daemon's fallback
  // kill. The supervisor itself enforces cancellation without the daemon.
  // The state update is serialized with cancellation.
  const auto directory = journal.job_path(job);
  const auto work = directory / "work";
  private_directory(work);
  const auto group = cgroup_path(cgroup_root, journal, job);
  pid_t child = -1;
  R::JobState outcome = R::JobState::Failed;
  std::string result, diagnostic;
  try {
    journal.supervisor_identity(job, static_cast<uint64_t>(::getpid()),
                                process_start(::getpid()), boot());
    const auto spec = parse_spec(journal.spec(job));
    fs::write_atomic(work / "spec.fb", journal.spec(job), 0600);
    if (spec->cpu_millis) {
      if (cgroup_root.empty())
        throw std::runtime_error("resource limits require a delegated cgroup");
      make_cgroup(group, *spec);
    }
    Fd adapter(::open((work / "adapter.log").c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (adapter.value < 0) throw std::runtime_error("cannot open adapter log");
    const pid_t parent = ::getpid();
    child = ::fork();
    if (child < 0) throw std::runtime_error("cannot start isolated worker");
    if (child == 0) {
      if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent)
        ::_exit(126);
      ::dup2(adapter.value, 1);
      ::dup2(adapter.value, 2);
      if (spec->cpu_millis) {
        int fd = ::open((group / "cgroup.procs").c_str(), O_WRONLY);
        if (fd < 0 || ::write(fd, "0", 1) != 1) ::_exit(126);
        ::close(fd);
      }
      const auto blobs = directory / "blobs";
      const auto log_limit = std::to_string(log_limit_bytes);
      ::execlp("unshare", "unshare", "--map-current-user", "--pid", "--fork",
               "--kill-child=SIGKILL", "--mount-proc", "--", self.c_str(),
               "--invoke", work.c_str(), blobs.c_str(), store.c_str(), log_limit.c_str(), nullptr);
      ::_exit(127);
    }
    int status = 0;
    for (;;) {
      const auto current = journal.status(job);
      if (current.state == R::JobState::Cancelling)
        throw std::runtime_error("cancelled");
      if (now_ms() >= current.deadline)
        throw std::runtime_error("timed out after " +
                                 std::to_string(spec->timeout_seconds) + " s");
      const auto done = ::waitpid(child, &status, WNOHANG);
      if (done == child) {
        child = -1;
        break;
      }
      if (done < 0 && errno != EINTR)
        throw std::runtime_error("cannot wait for worker");
      std::this_thread::sleep_for(100ms);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      const auto size = stdfs::file_size(work / "adapter.log");
      diagnostic = fs::read_range(work / "adapter.log",
                                  size > 2048 ? size - 2048 : 0, 2048)
                       .value_or("");
      throw std::runtime_error("worker failed: " + diagnostic);
    }
    result = journal.import_results(job);
    if (now_ms() >= journal.status(job).deadline)
      throw std::runtime_error("timed out after " +
                               std::to_string(spec->timeout_seconds) + " s");
    outcome = R::JobState::Succeeded;
  } catch (const std::exception& e) {
    diagnostic = e.what();
  }
  if (child > 0) {
    ::kill(child, SIGKILL);
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
  }
  remove_cgroup(group);
  try {
    discard_tree(work);
  } catch (const std::exception& e) {
    log(e.what());
  }
  journal.finish(job, outcome, result, diagnostic);
  return 0;
}

auto unix_listener(const stdfs::path& path) -> int {
  sockaddr_un address{};
  const auto text = path.string();
  if (!path.is_absolute() || text.size() >= sizeof address.sun_path)
    throw std::runtime_error("invalid Unix socket path");
  // The singleton lock owns this private endpoint; never unlink arbitrary
  // files.
  struct stat st{};
  if (::lstat(path.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid())
      throw std::runtime_error("Unix endpoint is not an owned socket");
    ::unlink(path.c_str());
  }
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, text.data(), text.size());
  if (fd < 0 ||
      ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
      ::listen(fd, 64) != 0)
    throw std::runtime_error("cannot listen on Unix socket");
  ::chmod(path.c_str(), 0600);
  return fd;
}
auto tcp_listener(const Config& config) -> int {
  const bool v6 = config.host.find(':') != std::string::npos;
  int fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  sockaddr_in6 b{};
  a.sin_family = AF_INET;
  a.sin_port = htons(config.port);
  const bool ipv4 = ::inet_pton(AF_INET, config.host.c_str(), &a.sin_addr) == 1;
  b.sin6_family = AF_INET6;
  b.sin6_port = htons(config.port);
  const bool ipv6 =
      ::inet_pton(AF_INET6, config.host.c_str(), &b.sin6_addr) == 1;
  if (!(v6 ? ipv6 : ipv4))
    throw std::runtime_error("--host must be an IP literal");
  if (fd < 0 ||
      ::bind(fd,
             v6 ? reinterpret_cast<sockaddr*>(&b)
                : reinterpret_cast<sockaddr*>(&a),
             v6 ? sizeof b : sizeof a) != 0 ||
      ::listen(fd, 64) != 0)
    throw std::runtime_error("cannot listen on TLS TCP");
  return fd;
}
auto run(Config config) -> int {
  private_directory(config.root);
  Fd singleton(::open((config.root / "worker.lock").c_str(),
                      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (singleton.value < 0 || ::flock(singleton.value, LOCK_EX | LOCK_NB) != 0)
    throw std::runtime_error("another remote worker owns this state directory");
  if (config.unix_socket.empty())
    config.unix_socket = config.root / "worker.sock";
  peer_tls::Identity identity(config.root / "identity.pem");
  if (config.print_key) {
    std::cout << identity.public_key() << '\n';
    return 0;
  }
  if (config.authorized_keys.empty())
    config.authorized_keys = config.root / "authorized_keys";
  if (!stdfs::exists(config.authorized_keys))
    fs::write_atomic(config.authorized_keys,
                     "# One ed25519: SPKI key per line.\n", 0600);
  peer_tls::Context tls(identity, true, [&](std::string_view key) {
    return peer_tls::Authorized(config.authorized_keys, key);
  });
  Daemon daemon(config);
  Fd unixfd(unix_listener(config.unix_socket));
  Fd tcpfd(tcp_listener(config));
  ::signal(SIGTERM, stop);
  ::signal(SIGINT, stop);
  ::signal(SIGPIPE, SIG_IGN);
  std::atomic<size_t> active{0};
  struct Client {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<Client> clients;
  log("public key: " + identity.public_key());
  log("listening with TLS 1.3 on unix:" + config.unix_socket.string() +
      " and tls://" + config.host + ":" + std::to_string(config.port));
  while (!stopping) {
    daemon.tick();
    for (auto i = clients.begin(); i != clients.end();) {
      if (i->done->load()) {
        i->thread.join();
        i = clients.erase(i);
      } else
        ++i;
    }
    pollfd listeners[2]{{unixfd.value, POLLIN, 0}, {tcpfd.value, POLLIN, 0}};
    if (::poll(listeners, 2, 100) <= 0) continue;
    for (auto& listener : listeners)
      if (listener.revents & POLLIN) {
        const int fd = ::accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        if (active.load() >= kConnections) {
          ::close(fd);
          continue;
        }
        ++active;
        auto done = std::make_shared<std::atomic<bool>>(false);
        clients.push_back({std::thread([&, fd, done] {
                             Fd connection(fd);
                             try {
                               peer_tls::ConfigureSocket(fd, 15);
                               peer_tls::Stream stream(tls, fd, true);
                               daemon.serve_stream(stream);
                             } catch (const std::exception&) {
                             }
                             --active;
                             done->store(true);
                           }),
                           done});
      }
  }
  for (auto& client : clients) client.thread.join();
  ::unlink(config.unix_socket.c_str());
  // Detached supervisors and their namespaces continue through listener
  // restarts.
  return 0;
}
}  // namespace
}  // namespace tsugumi::workerd

auto main(int argc, char** argv) -> int {
  using namespace tsugumi::workerd;
  ::umask(0077);
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--check-namespace")
      return ::getpid() == 1 ? 0 : 1;
    if (argc == 6 && std::string_view(argv[1]) == "--invoke") {
      invoke(argv[2], argv[3], argv[4], remote::checked_log_limit(parse_log_limit_bytes(argv[5])));
      return 0;
    }
    const auto self = stdfs::canonical("/proc/self/exe").string();
    if (argc == 8 && std::string_view(argv[1]) == "--supervise")
      return supervise(self, argv[2], argv[3], std::stoi(argv[4]), argv[5],
                       argv[6], remote::checked_log_limit(parse_log_limit_bytes(argv[7])));
    Config c;
    c.root = default_root();
    c.executable = self;
    for (int i = 1; i < argc; ++i) {
      const std::string_view option(argv[i]);
      if (option == "--public-key") {
        c.print_key = true;
        continue;
      }
      if (++i >= argc) throw std::runtime_error("expected option and value");
      const std::string value(argv[i]);
      if (option == "--state-dir")
        c.root = value;
      else if (option == "--socket")
        c.unix_socket = value;
      else if (option == "--authorized-keys")
        c.authorized_keys = value;
      else if (option == "--host")
        c.host = value;
      else if (option == "--port") {
        const auto n = std::stoul(value);
        if (!n || n > 65535) throw std::runtime_error("invalid port");
        c.port = static_cast<uint16_t>(n);
      } else if (option == "--max-jobs") {
        const auto n = std::stoul(value);
        if (!n || n > 65536) throw std::runtime_error("invalid max-jobs");
        c.max_jobs = static_cast<uint32_t>(n);
      } else if (option == "--log-limit-bytes")
        c.log_limit_bytes = remote::checked_log_limit(parse_log_limit_bytes(value));
      else if (option == "--cgroup-root")
        c.cgroup = value;
      else
        throw std::runtime_error("unknown remote worker option");
    }
    return run(std::move(c));
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
