// Remote reception and detached supervision share one binary. Only --invoke
// initialises Nix; listeners and carriers operate on bounded protocol frames.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <termios.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "dashboard.hh"
#include "display_channel.hh"
#include "jobs_command.hh"
#include "journal.hh"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/rpc/password.hh"
#include "tsugumi/rpc/peer_tls.hh"
#include "tsugumi/rpc/unix_socket.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/invocation.hh"
#include "tsugumi/workerd/path_ids.hh"
#include "tsugumi/workerd/worker_proc.hh"
#include "worker_settings.hh"

extern char** environ;
namespace tsugumi::workerd {
auto invoke(const stdfs::path& directory, const stdfs::path& blobs,
            const stdfs::path& store, const stdfs::path& group,
            const NixBuildLimits& limits) -> void;
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
  Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
  auto operator=(const Fd&) -> Fd& = delete;
  ~Fd() {
    if (value >= 0) ::close(value);
  }
};

auto relay_cache_files(int channel, int stopping_fd, peer_tls::Stream& tls)
    -> void {
  namespace frame = tsugumi::rpc::frame;
  const auto response = [&] {
    auto bytes = tls.Read(frame_length(tls.Read(4)));
    auto message = decode(bytes);
    const auto* reply = message->payload.AsReply();
    const auto* file = reply ? reply->body.AsCacheFile() : nullptr;
    frame::Write(channel, bytes, kMaxFrame);
    if (!file) throw std::runtime_error("binary cache relay ended");
    return *file;
  };
  for (;;) {
    pollfd wait[]{{channel, POLLIN, 0}, {stopping_fd, POLLIN, 0}};
    const auto signalled = ::poll(wait, 2, -1);
    if (signalled < 0 && errno == EINTR) continue;
    if (signalled <= 0 || wait[1].revents) return;
    auto bytes = frame::Read(channel, kMaxFrame);
    if (!bytes) return;
    auto message = decode(*bytes);
    auto* request = message->payload.AsRequest();
    auto* file = request ? request->body.AsCacheFileRequest() : nullptr;
    if (!file) throw std::runtime_error("invalid binary cache relay request");
    tls.Write(length_prefix(static_cast<uint32_t>(bytes->size())) + *bytes);
    auto result = response();
    if (file->method == R::CacheMethod::Put && result.status == 100) {
      std::string chunk;
      for (auto remaining = file->size; remaining > 0;) {
        chunk.resize(std::min<uint64_t>(remaining, kChunkBytes));
        if (frame::ReadExact(channel, chunk.data(), chunk.size()) !=
            chunk.size())
          throw std::runtime_error("incomplete binary cache upload");
        tls.Write(chunk);
        remaining -= chunk.size();
      }
      result = response();
    } else if (file->method == R::CacheMethod::Get && result.status == 200) {
      for (auto remaining = result.size; remaining > 0;) {
        const auto chunk = tls.Read(std::min<uint64_t>(remaining, kChunkBytes));
        frame::WriteAll(channel, chunk.data(), chunk.size());
        remaining -= chunk.size();
      }
    }
  }
}
// The adapter and the project each have an independent credit windows per
// session and route.
class DisplayChannel {
 public:
  explicit DisplayChannel(Fd&& descriptor)
      : socket_(std::move(descriptor)), reader_([this] { read(); }) {
    try {
      writer_ = std::thread([this] { write(); });
    } catch (...) {
      ::shutdown(socket_.value, SHUT_RDWR);
      reader_.join();
      throw;
    }
  }
  ~DisplayChannel() {
    ::shutdown(socket_.value, SHUT_RDWR);
    reader_.join();
    writer_.join();
  }

  auto take(R::JobStatusT& status, const R::StatusRequestT& query) -> void {
    std::lock_guard lock(mutex_);
    if (query.display_after == sequence_) replay_.reset();
    if (!closed_ && request_ && query.view_reply &&
        query.view_reply->request_id == request_->request_id) {
      response_ = *query.view_reply;
      request_.reset();
      ready_.notify_all();
    }
    if (!closed_ && request_)
      status.view_request = std::make_unique<W::WorkerViewRequestT>(*request_);
    for (const auto& credit : query.display_credits) {
      const auto view = outbound_.find(view_uuid(credit->address.get()));
      if (view != outbound_.end() && credit->released <= view->second.sent)
        view->second.released =
            std::max(view->second.released, credit->released);
    }
    if (!replay_) {
      const auto next = std::ranges::find_if(pending_, [&](const auto& item) {
        const auto& view = outbound_.at(view_uuid(item.session.get()));
        return view.sent - view.released <= kViewBudget - TSUGUMI_MESSAGE_MAX;
      });
      if (next != pending_.end()) {
        auto& view = outbound_.at(view_uuid(next->session.get()));
        const auto size = message_cost(next->bytes.size());
        view.queued -= size;
        view.sent += size;
        if (!closed_) {
          view.credit += size;
          ready_.notify_all();
        }
        replay_.emplace(std::move(*next));
        pending_.erase(next);
        ++sequence_;
      }
    }
    if (replay_) {
      status.displays.push_back(std::make_unique<W::WorkerDisplayT>(*replay_));
      status.display_sequence = sequence_;
    }
    status.view_input_ready = input_ready_ && !closed_;
    for (const auto& [route, view] : inbound_) {
      auto credit = std::make_unique<R::DisplayCreditT>();
      credit->address = std::make_unique<W::ViewUuid>(view_uuid(route));
      credit->released = view.released;
      status.view_input_credits.push_back(std::move(credit));
    }
    status.display_pending =
        !pending_.empty() ||
        (status.state == R::JobState::Succeeded && !closed_);
  }

  auto send(const R::ViewSendRequestT& request) -> bool {
    const auto& raw = *request.event;
    WorkerViewEvent event{static_cast<ViewEventKind>(raw.kind),
                          view_uuid(raw.route.get()),
                          view_uuid(raw.session.get()),
                          std::string(raw.bytes.begin(), raw.bytes.end())};
    if (!valid_view_event(event)) return false;
    std::lock_guard lock(mutex_);
    if (!input_ready_ || closed_) return false;
    auto& route = inbound_[event.route];
    if (event.kind == ViewEventKind::close) {
      if (std::exchange(route.closed, true)) return true;
    } else {
      if (request.offset < route.accepted)
        return true;  // Retried after a lost reply.
      if (route.closed || request.offset != route.accepted ||
          route.queued > kViewBudget - TSUGUMI_MESSAGE_MAX)
        return false;
      const auto cost = message_cost(event.bytes.size());
      route.queued += cost;
      route.accepted += cost;
    }
    route.pending.push_back(std::move(event));
    ready_.notify_all();
    return true;
  }

  auto expired() -> bool {
    std::lock_guard lock(mutex_);
    return closed_ && std::chrono::steady_clock::now() - *closed_ > 10s &&
           pending_.empty() && !replay_;
  }

 private:
  struct Outbound {
    size_t queued = 0;
    size_t credit = 0;
    uint64_t sent = 0;
    uint64_t released = 0;
  };
  struct Inbound {
    std::deque<WorkerViewEvent> pending;
    size_t queued = 0;
    size_t in_flight = 0;
    uint64_t accepted = 0, released = 0;
    bool closed = false;
  };

  auto read() -> void {
    try {
      char ready = 0;
      if (::read(socket_.value, &ready, 1) == 1 && ready == 1) {
        {
          std::lock_guard lock(mutex_);
          input_ready_ = true;
        }
        while (auto frame = read_display(socket_.value)) {
          const auto* envelope = W::GetWorkerMessage(frame->data());
          std::lock_guard lock(mutex_);
          if (const auto* request = envelope->value_as_WorkerViewRequest()) {
            if (request_ || response_ || request->request_id() <= request_id_ ||
                request->kind() > W::ViewControlKind::Close)
              break;
            request_id_ = request->request_id();
            request_.emplace();
            request->UnPackTo(&*request_);
            continue;
          }
          if (const auto* credit = envelope->value_as_WorkerDisplayCredit()) {
            const auto view = inbound_.find(view_uuid(credit->address()));
            if (view == inbound_.end() || credit->bytes() > view->second.in_flight)
              break;
            view->second.in_flight -= credit->bytes();
            ready_.notify_all();
            continue;
          }
          const auto* message = envelope->value_as_WorkerDisplay();
          if (!message || view_uuid(message->session()) == ViewUuid{} ||
              message->bytes()->size() > TSUGUMI_MESSAGE_MAX)
            break;
          const auto size = message_cost(message->bytes()->size());
          auto& view = outbound_[view_uuid(message->session())];
          if (view.queued > kViewBudget - TSUGUMI_MESSAGE_MAX) break;
          view.queued += size;
          pending_.emplace_back();
          message->UnPackTo(&pending_.back());
        }
      }
    } catch (const std::exception&) {
    }
    std::lock_guard lock(mutex_);
    closed_ = std::chrono::steady_clock::now();
    ready_.notify_all();
    ::shutdown(socket_.value, SHUT_RDWR);
  }

  auto write() -> void {
    ViewUuid last{};
    const auto has_credit = [](const auto& entry) {
      return entry.second.credit != 0;
    };
    const auto has_capacity = [](const Inbound& route) {
      return route.in_flight <= kViewBudget - TSUGUMI_MESSAGE_MAX;
    };
    const auto can_send = [&](const auto& entry) {
      const auto& route = entry.second;
      return !route.pending.empty() &&
             (route.pending.front().kind == ViewEventKind::close ||
              has_capacity(route));
    };
    try {
      for (;;) {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] {
          return closed_ || response_ ||
                 std::ranges::any_of(outbound_, has_credit) ||
                 std::ranges::any_of(inbound_, can_send);
        });
        if (closed_) return;
        if (response_) {
          const auto response = std::exchange(response_, std::nullopt);
          lock.unlock();
          flatbuffers::FlatBufferBuilder b;
          build_view_reply(b, {response->request_id, response->status,
                               view_uuid(response->session.get()),
                               view_uuid(response->route.get())});
          write_display_frame(socket_.value, b);
          continue;
        }
        // Credit can unblock the other direction even when data cannot move.
        const auto credit_entry = std::ranges::find_if(outbound_, has_credit);
        if (credit_entry != outbound_.end()) {
          const auto view = credit_entry->first;
          const auto credit = std::exchange(credit_entry->second.credit, 0);
          lock.unlock();
          write_display_credit(socket_.value, view, credit);
          continue;
        }
        const auto end = inbound_.end();
        auto next = std::find_if(inbound_.upper_bound(last), end, can_send);
        if (next == end) next = std::find_if(inbound_.begin(), end, can_send);
        last = next->first;
        auto& route = next->second;
        auto event = std::move(route.pending.front());
        route.pending.pop_front();
        if (event.kind == ViewEventKind::message) {
          const auto cost = message_cost(event.bytes.size());
          route.queued -= cost;
          route.released += cost;
          route.in_flight += cost;
        }
        lock.unlock();
        flatbuffers::FlatBufferBuilder b;
        build_view_event(b, event);
        write_display_frame(socket_.value, b);
      }
    } catch (const std::exception&) {
      ::shutdown(socket_.value, SHUT_RDWR);
    }
  }

  Fd socket_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<W::WorkerDisplayT> pending_;
  std::optional<W::WorkerDisplayT> replay_;
  uint64_t sequence_ = 0;
  std::map<ViewUuid, Outbound> outbound_;
  std::map<ViewUuid, Inbound> inbound_;
  uint64_t request_id_ = 0;
  std::optional<W::WorkerViewRequestT> request_;
  std::optional<W::WorkerViewReplyT> response_;
  bool input_ready_ = false;
  std::optional<std::chrono::steady_clock::time_point> closed_;
  std::thread reader_;
  std::thread writer_;
};

auto log(const std::string& line) -> void {
  std::cerr << "tsugumi-workerd: " << line << '\n';
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
auto process_fd(pid_t pid) -> Fd {
  return Fd(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
}
auto supervisor_process(const Record& state) -> Fd {
  if (state.pid <= 1 || state.boot != boot()) return {};
  const auto pid = static_cast<pid_t>(state.pid);
  auto fd = process_fd(pid);
  if (fd.value >= 0) {
    try {
      if (process_start(pid) == state.start) return fd;
    } catch (const std::exception&) { /* The recorded process already exited. */
    }
  }
  return {};
}
auto notify_cancel(const Record& state) -> void {
  const auto pidfd = supervisor_process(state);
  if (pidfd.value >= 0 &&
      ::syscall(SYS_pidfd_send_signal, pidfd.value, SIGUSR1, nullptr, 0) != 0 &&
      errno != ESRCH)
    log("cannot notify execution supervisor: " +
        std::string(std::strerror(errno)));
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
                 const Sha256Hex& job) -> stdfs::path {
  if (root.empty()) return {};
  return root / ("tsugumi-" + crypto::sha256_hex(journal.root().string() +
                                                 std::string(job.view()))
                                  .substr(0, 32));
}
auto supervisor_lock(const Journal& journal, const Sha256Hex& job,
                     bool notify_on_exit = false) -> Fd {
  const int mode = notify_on_exit ? O_RDWR : O_RDONLY;
  return Fd(::open(journal.supervisor_lock_path(job).c_str(),
                   mode | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
}
auto try_supervisor_lock(const Fd& lock) -> bool {
  if (lock.value < 0) throw std::runtime_error("cannot open supervisor lock");
  int rc;
  do {
    rc = ::flock(lock.value, LOCK_EX | LOCK_NB);
  } while (rc != 0 && errno == EINTR);
  if (rc == 0) return true;
  if (errno == EWOULDBLOCK) return false;
  throw std::system_error(errno, std::generic_category(),
                          "cannot acquire supervisor lock");
}

struct Config {
  stdfs::path root;
  stdfs::path unix_socket;
  bool print_key = false;
  stdfs::path cgroup;
  std::string executable;
  // The command line, to start again with on a restart from the dashboard.
  std::vector<std::string> arguments;
};
// The source revision this worker was built from. The flake's package sets
// it in the wrapper; a build that does not say reports none.
auto build_revision() -> std::string {
  const char* value = std::getenv("TSUGUMI_WORKERD_REVISION");
  const std::string_view revision = value ? value : "";
  if (revision.empty() || revision.size() > 64 ||
      !std::ranges::all_of(revision, [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '.';
      }))
    return {};
  return std::string(revision);
}
enum class Restart { None, AfterJobs, Now };
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
  int status = 0;
  pid_t done;
  do {
    done = ::waitpid(pid, &status, 0);
  } while (done < 0 && errno == EINTR);
  if (done < 0) throw std::runtime_error("cannot wait for namespace check");
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
  Daemon(Config config, WorkerSettings& settings, std::string dashboard_sha256,
         int shutdown_fd)
      : config_(std::move(config)),
        server_(identity(config_.root, "server_id")),
        revision_(build_revision()),
        settings_(settings),
        system_(system()),
        dashboard_(config_.root, settings_,
                   {.system = system_,
                    .revision = revision_,
                    .running = [this] { return running_jobs_.load(); },
                    .restart = [this](bool now) { request_restart(now); },
                    .restart_pending =
                        [this] { return restart_.load() != Restart::None; },
                    .last_seen = [this] { return last_seen(); }}),
        dashboard_sha256_(std::move(dashboard_sha256)),
        shutdown_fd_(shutdown_fd),
        journal_events_(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
    if (journal_events_.value < 0)
      throw std::runtime_error("cannot create journal event descriptor");
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
    c.max_jobs = settings_.max_jobs();
    c.label = settings_.saved().name;
    c.dashboard_port = dashboard_port_;
    c.dashboard_certificate_sha256 = dashboard_sha256_;
    c.active_jobs = active_jobs_.load();
    c.probe_life = probe_life_;
    c.worker_contract = kContract;
    c.revision = revision_;
    return c;
  }
  auto project(const Sha256Hex& scope) -> std::shared_ptr<Journal> {
    std::lock_guard guard(project_mutex_);
    if (auto existing = projects_.find(scope); existing != projects_.end())
      return existing->second;
    auto journal =
        std::make_shared<Journal>(config_.root / "projects" / scope.view());
    const int watch = ::inotify_add_watch(journal_events_.value,
                                          (journal->root() / "locks").c_str(),
                                          IN_CLOSE_WRITE | IN_ONLYDIR);
    if (watch < 0)
      throw std::system_error(errno, std::generic_category(),
                              "cannot watch journal locks");
    journal_watches_.emplace(watch, journal);
    projects_.emplace(scope, journal);
    return journal;
  }
  auto handle(R::RequestT& q, Fd& cache_relay) -> R::MessageT {
    if (q.body.type == R::RequestBody::NONE ||
        q.body.type == R::RequestBody::CatalogRequest)
      throw Error(R::ErrorCode::Invalid, "invalid remote request");
    if (q.project_scope.empty()) {
      if (q.body.type != R::RequestBody::ProbeRequest || !q.instance_id.empty())
        throw Error(R::ErrorCode::Rejected,
                    "host-authorized project scope required");
      return reply(capabilities());
    }
    const auto journal =
        project(require_sha256_hex(q.project_scope, "project scope"));
    if ((!q.instance_id.empty() && q.instance_id != journal->instance()) ||
        (q.body.type != R::RequestBody::ProbeRequest && q.instance_id.empty()))
      throw Error(R::ErrorCode::InstanceChanged,
                  "project journal identity changed");
    if (q.body.type == R::RequestBody::ProbeRequest) {
      return reply(capabilities(journal->instance()));
    }
    const auto job = require_sha256_hex(q.job_id, "job ID");
    const auto with_log_status = [&](R::JobStatusT status) {
      std::error_code ec;
      status.log_truncated = std::filesystem::is_regular_file(
          journal->job_path(job) / "work/log-truncated", ec);
      return status;
    };
    const auto log_slice = [&](uint64_t offset, uint32_t limit) {
      auto range = journal->log(job, offset, std::min(limit, kLogChunkBytes),
                                q.core_life_id);
      return [offset, range = std::move(range)](R::JobStatusT status) {
        status.log_offset = offset;
        status.log_size = range.first;
        status.log_bytes.assign(range.second.begin(), range.second.end());
        return status;
      };
    };
    switch (q.body.type) {
      case R::RequestBody::PutBlobRequest: {
        const auto& chunk = *q.body.AsPutBlobRequest();
        const auto digest = require_sha256_hex(chunk.digest, "blob digest");
        journal->put(job, digest, chunk.total, chunk.offset, chunk.bytes,
                     q.core_life_id);
        return reply(R::AckT{});
      }
      case R::RequestBody::GetBlobRequest: {
        const auto& request = *q.body.AsGetBlobRequest();
        const auto digest = require_sha256_hex(request.digest, "blob digest");
        return reply(journal->get(job, digest, request.offset, request.limit,
                                  q.core_life_id));
      }
      case R::RequestBody::SubmitRequest: {
        auto& request = *q.body.AsSubmitRequest();
        const auto& bytes = request.spec;
        if (!request.caches || !request.caches->push)
          throw Error(R::ErrorCode::Invalid, "missing job cache settings");
        const std::string_view data(reinterpret_cast<const char*>(bytes.data()),
                                    bytes.size());
        const auto spec = parse_spec(data);
        validate_log_limit(*spec);
        validate_secret_names(*spec);
        if (spec->system != system_)
          throw Error(R::ErrorCode::Rejected,
                      "job system is not supported by this worker");
        if (!spec->pid)
          throw Error(R::ErrorCode::Rejected,
                      "remote execution requires PID isolation");
        if (!spec->filesystem)
          throw Error(R::ErrorCode::Rejected,
                      "remote execution requires filesystem isolation");
        if (spec->cpu_millis > 256'000 ||
            spec->memory_bytes > (uint64_t{1} << 40) ||
            (spec->memory_bytes && spec->memory_bytes < (uint64_t{1} << 20)))
          throw Error(R::ErrorCode::Rejected, "invalid sandbox limits");
        if ((spec->cpu_millis || spec->memory_bytes) && config_.cgroup.empty())
          throw Error(R::ErrorCode::Rejected,
                      "sandbox limits require a delegated cgroup v2 subtree");
        std::lock_guard guard(channel_mutex_);
        const auto submitted = journal->submit(
            job, data, parse_input_blobs(*spec), q.core_life_id);
        const auto channel = channel_key(*journal, job);
        if (submitted.state == R::JobState::Queued &&
            !cache_settings_.contains(channel)) {
          Fd invocation;
          if (request.caches->push->kind == R::CacheTargetKind::Own) {
            int pair[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
              throw std::runtime_error("cannot open binary cache relay");
            cache_relay.value = pair[0];
            invocation.value = pair[1];
          }
          cache_settings_.try_emplace(
              channel,
              PendingCache{std::move(request.caches), std::move(invocation)});
        }
        return reply(submitted.status());
      }
      case R::RequestBody::ViewSendRequest: {
        const auto record = journal->status(job, q.core_life_id);
        const auto channel = display_channel(*journal, job);
        if (record.state != R::JobState::Running || !channel ||
            !channel->send(*q.body.AsViewSendRequest()))
          throw Error(R::ErrorCode::Rejected, "the Card cannot accept a view message");
        return reply(R::AckT{});
      }
      case R::RequestBody::StatusRequest: {
        auto status =
            with_log_status(journal->status(job, q.core_life_id).status());
        const auto& query = *q.body.AsStatusRequest();
        status =
            log_slice(query.log_offset, query.log_limit)(std::move(status));
        if (status.state == R::JobState::Queued) {
          std::lock_guard guard(channel_mutex_);
          status.cache_settings_wanted =
              !cache_settings_.contains(channel_key(*journal, job));
        }
        if (status.state == R::JobState::Running) {
          std::error_code ec;
          if (const auto progress = remote::decode<R::JobProgress>(
                  fs::read_file(journal->job_path(job) / "work" / kActivity)
                      .value_or(""))) {
            status.activity = progress->activity;
            status.activity_detail = progress->detail;
            status.building = progress->building;
            status.cpu_ms = progress->cpu_ms;
          }
          status.secrets_wanted = stdfs::exists(
              journal->job_path(job) / "work" / kSecretsWanted, ec);
          if (!stdfs::exists(journal->job_path(job) / "work/artifact-reply",
                             ec))
            status.artifact_wanted =
                fs::read_file(journal->job_path(job) / "work/artifact-wanted")
                    .value_or("");
        }
        if (const auto channel = display_channel(*journal, job))
          channel->take(status, query);
        return reply(std::move(status));
      }
      case R::RequestBody::CancelRequest: {
        const auto& query = *q.body.AsCancelRequest();
        // Check the reader before cancelling; the slice may predate this
        // cancellation, and the next one continues from its end.
        std::function<R::JobStatusT(R::JobStatusT)> slice =
            [](R::JobStatusT status) { return status; };
        if (query.log_limit)
          slice = log_slice(query.log_offset, query.log_limit);
        const auto state = journal->cancel(job, query.fingerprint);
        if (state.state == R::JobState::Cancelling) notify_cancel(state);
        return reply(slice(with_log_status(state.status())));
      }
      case R::RequestBody::ReleaseRequest: {
        journal->release(job, q.body.AsReleaseRequest()->fingerprint);
        decltype(displays_)::node_type removed;
        {
          std::lock_guard guard(channel_mutex_);
          removed = displays_.extract(channel_key(*journal, job));
        }
        return reply(R::AckT{});
      }
      case R::RequestBody::JobArtifactRequest:
        journal->deliver_artifact(job, *q.body.AsJobArtifactRequest(),
                                  q.core_life_id);
        return reply(R::AckT{});
      case R::RequestBody::JobSecretsRequest:
        deliver_secrets(*journal, job, q.core_life_id,
                        *q.body.AsJobSecretsRequest());
        return reply(R::AckT{});
      default:
        throw Error(R::ErrorCode::Invalid, "invalid request body");
    }
  }
  auto serve_stream(peer_tls::Stream& stream) -> void {
    {
      std::lock_guard guard(seen_mutex_);
      seen_[stream.peer_key()] = std::chrono::system_clock::now();
    }
    try {
      auto n = frame_length(stream.Read(4));
      auto bytes = stream.Read(n);
      auto m = decode(bytes);
      clear_secret(bytes);
      auto* q = m->payload.AsRequest();
      if (!q) throw Error(R::ErrorCode::Invalid, "expected a request");
      Fd cache_relay;
      auto response = encode(handle(*q, cache_relay));
      stream.Write(length_prefix(static_cast<uint32_t>(response.size())) +
                   response);
      if (cache_relay.value >= 0)
        relay_cache_files(cache_relay.value, shutdown_fd_, stream);
    } catch (const Error& e) {
      auto bytes = encode(failure(e.code, e.what()));
      stream.Write(length_prefix(static_cast<uint32_t>(bytes.size())) + bytes);
    }
  }
  auto serve_dashboard(peer_tls::Stream& stream) -> void {
    dashboard_.serve(stream);
  }
  // Called once the listeners are bound, before any connection is served.
  auto listening(uint16_t port, uint16_t dashboard_port) -> void {
    dashboard_port_ = dashboard_port;
    dashboard_.listening(port, dashboard_port);
  }
  auto last_seen() -> std::map<std::string, std::chrono::system_clock::time_point> {
    std::lock_guard guard(seen_mutex_);
    return seen_;
  }
  auto request_restart(bool now) -> void {
    auto expected = Restart::None;
    if (!restart_.compare_exchange_strong(
            expected, now ? Restart::Now : Restart::AfterJobs) &&
        now)
      restart_.store(Restart::Now);
  }
  // On the main thread: whether the listener should start again now. A
  // restart "now" first cancels the running jobs, as a host's cancel would.
  auto restart_due() -> bool {
    const auto mode = restart_.load();
    if (mode == Restart::None) return false;
    if (mode == Restart::AfterJobs)
      return running_.empty() && handoffs_.empty();
    for (const auto& key : running_) cancel_for_restart(key.first);
    for (const auto& key : handoffs_) cancel_for_restart(key);
    return true;
  }
  auto recover() -> void {
    for (const auto& entry :
         stdfs::directory_iterator(config_.root / "projects")) {
      const auto scope = Sha256Hex::parse(entry.path().filename().string());
      if (!scope) continue;
      const auto journal = project(*scope);
      for (const auto& job : journal->jobs()) refresh({journal, job});
    }
    dispatch();
  }
  auto watch_events(std::vector<pollfd>& fds) const -> void {
    fds.push_back({journal_events_.value, POLLIN, 0});
    for (const auto& [_, process] : running_)
      fds.push_back({process.value, POLLIN, 0});
  }
  auto tick(std::span<const pollfd> events) -> void {
    {
      // Destruction joins the channel threads. Keep references until the map
      // mutex is free so other requests can still find their channels.
      std::vector<std::shared_ptr<DisplayChannel>> expired;
      std::lock_guard guard(channel_mutex_);
      std::erase_if(displays_, [&](const auto& item) {
        if (!item.second->expired()) return false;
        expired.push_back(item.second);
        return true;
      });
    }
    while (::waitpid(-1, nullptr, WNOHANG) > 0) {
    }
    // The timeout also covers a lock-close notification arriving just
    // before the kernel releases its flock. Only unresolved handoffs retry.
    // Stable supervisors have pidfds, including across listener restarts.
    std::set<Job> dirty;
    dirty.swap(handoffs_);
    size_t index = 1;
    for (auto it = running_.begin(); it != running_.end();) {
      if (events[index++].revents) {
        dirty.insert(it->first);
        it = running_.erase(it);
      } else {
        ++it;
      }
    }
    if (events.front().revents && journal_changes(dirty)) {
      // If the kernel dropped notifications, rebuild from disk before using
      // the queue. Watches are already in place for subsequent changes.
      recover();
      dirty.clear();
    }
    for (const auto& job : dirty) refresh(job);
    dispatch();
  }

 private:
  using Job = std::pair<std::shared_ptr<Journal>, Sha256Hex>;
  auto journal_changes(std::set<Job>& dirty) -> bool {
    alignas(inotify_event) char bytes[4096];
    bool overflow = false;
    for (;;) {
      const auto size = ::read(journal_events_.value, bytes, sizeof(bytes));
      if (size < 0 && errno == EINTR) continue;
      if (size < 0 && errno == EAGAIN) return overflow;
      if (size <= 0) throw std::runtime_error("cannot read journal events");
      for (size_t offset = 0; offset < static_cast<size_t>(size);) {
        const auto& event =
            *reinterpret_cast<const inotify_event*>(bytes + offset);
        offset += sizeof(event) + event.len;
        if (event.mask & IN_Q_OVERFLOW) overflow = true;
        // Supervisor lock closures wake the poll for handoffs that are
        // already dirty. Only journal lock names add a new job to the set.
        if (!(event.mask & IN_CLOSE_WRITE) || !event.len) continue;
        const auto job = Sha256Hex::parse(event.name);
        if (!job) continue;
        std::lock_guard guard(project_mutex_);
        if (const auto found = journal_watches_.find(event.wd);
            found != journal_watches_.end())
          dirty.emplace(found->second, *job);
      }
    }
  }
  auto refresh(const Job& key) -> void {
    if (running_.contains(key)) return;
    const auto& [p, job] = key;
    try {
      const auto lock = supervisor_lock(*p, job);
      const bool acquired = try_supervisor_lock(lock);
      const auto state = p->status(job);
      if (!acquired) {
        // A terminal record can still have a supervisor doing its last
        // writes. Its slot is occupied until the process exits.
        auto process = supervisor_process(state);
        if (process.value >= 0)
          running_.emplace(key, std::move(process));
        else
          handoffs_.insert(key);
        queued_.erase(key);
        return;
      }
      handoffs_.erase(key);
      close_channel(*p, job);
      if (state.state == R::JobState::Queued) {
        if (!queued_.contains(key)) {
          const auto submitted =
              stdfs::last_write_time(p->job_path(job) / "spec.fb");
          queued_.emplace(key, submitted);
        }
        return;
      }
      {
        std::lock_guard guard(channel_mutex_);
        cache_settings_.erase(channel_key(*p, job));
      }
      queued_.erase(key);
      if (state.state == R::JobState::Running ||
          state.state == R::JobState::Cancelling) {
        remove_card_cgroup(cgroup_path(config_.cgroup, *p, job));
        p->finish(job, R::JobState::Interrupted, {},
                  "execution supervisor ended without recording a result");
      } else if (terminal(state.state)) {
        remove_card_cgroup(cgroup_path(config_.cgroup, *p, job));
        if (state.state == R::JobState::Released) p->clean_released(job);
      }
    } catch (const std::exception& e) {
      queued_.erase(key);
      log("cannot recover job " + p->job_path(job).string() + ": " + e.what());
    }
  }
  auto cancel_for_restart(const Job& key) -> void {
    const auto& [p, job] = key;
    try {
      const auto record = p->status(job);
      if (record.state != R::JobState::Running &&
          record.state != R::JobState::Queued)
        return;
      const auto state = p->cancel(job, record.fingerprint);
      if (state.state == R::JobState::Cancelling) notify_cancel(state);
    } catch (const std::exception& e) {
      log("cannot cancel job " + p->job_path(job).string() +
          " for the restart: " + e.what());
    }
  }
  auto dispatch() -> void {
    const auto occupied = [&] { return running_.size() + handoffs_.size(); };
    // A pending restart starts no further job; queued jobs wait for the
    // listener that follows.
    while (!queued_.empty() && occupied() < settings_.max_jobs() && !stopping &&
           restart_.load() == Restart::None) {
      auto next = queued_.end();
      {
        std::lock_guard guard(channel_mutex_);
        for (auto it = queued_.begin(); it != queued_.end(); ++it) {
          const auto& [project, job] = it->first;
          if (cache_settings_.contains(channel_key(*project, job)) &&
              (next == queued_.end() || it->second < next->second))
            next = it;
        }
      }
      if (next == queued_.end()) break;
      auto pending = queued_.extract(next);
      const auto& [p, job] = pending.key();
      try {
        // Keep the writable lock description in the supervisor so its
        // closure also wakes the listener during the identity handoff.
        const auto lock = supervisor_lock(*p, job, true);
        if (!try_supervisor_lock(lock)) {
          refresh(pending.key());
          continue;
        }
        // A crash after claiming but before spawning leaves an interrupted
        // job, so the same invocation cannot run twice.
        if (!p->claim(job, 0, 0, {})) continue;
        const auto pid = spawn(*p, job, lock.value);
        if (pid < 0) {
          close_channel(*p, job);
          p->finish(job, R::JobState::Failed, {}, "supervisor launch failed");
        } else {
          auto process = process_fd(pid);
          if (process.value >= 0)
            running_.emplace(pending.key(), std::move(process));
          else
            handoffs_.insert(pending.key());
        }
      } catch (const std::exception& e) {
        log("cannot claim job " + p->job_path(job).string() + ": " + e.what());
      }
    }
    active_jobs_.store(static_cast<uint32_t>(occupied() + queued_.size()));
    running_jobs_.store(static_cast<uint32_t>(occupied()));
  }
  static auto channel_key(const Journal& journal, const Sha256Hex& job)
      -> std::string {
    return journal.root().string() + '\n' + std::string(job.view());
  }
  auto display_channel(const Journal& journal, const Sha256Hex& job)
      -> std::shared_ptr<DisplayChannel> {
    std::lock_guard guard(channel_mutex_);
    const auto found = displays_.find(channel_key(journal, job));
    return found == displays_.end() ? nullptr : found->second;
  }
  auto close_channel(const Journal& journal, const Sha256Hex& job) -> void {
    std::lock_guard guard(channel_mutex_);
    channels_.erase(channel_key(journal, job));
  }
  // Writes a running job's Card secrets into the pipe its invocation reads,
  // once, and closes it. Only this listener life holds the pipe: after a
  // restart the invocation reads end of file and fails, and nothing resends.
  auto deliver_secrets(const Journal& journal, const Sha256Hex& job,
                       const std::string& core, R::JobSecretsRequestT& body)
      -> void {
    const auto state = journal.status(job, core);
    if (state.fingerprint != body.fingerprint)
      throw Error(R::ErrorCode::Rejected,
                  "Card secrets were sent for a different job");
    std::error_code ec;
    if (state.state != R::JobState::Running ||
        !stdfs::exists(journal.job_path(job) / "work" / kSecretsWanted, ec))
      throw Error(R::ErrorCode::Rejected,
                  "the job is not waiting for its Card secrets");
    const auto spec = parse_spec(journal.spec(job));
    std::vector<std::string> wanted = spec->secrets, given;
    for (const auto& secret : body.secrets) given.push_back(secret->name);
    std::ranges::sort(wanted);
    std::ranges::sort(given);
    if (wanted != given)
      throw Error(R::ErrorCode::Rejected,
                  "Card secrets do not match the names the job declares");
    std::shared_ptr<Fd> channel;
    {
      std::lock_guard guard(channel_mutex_);
      const auto found = channels_.find(channel_key(journal, job));
      if (found != channels_.end()) {
        channel = std::move(found->second);
        channels_.erase(found);
      }
    }
    if (!channel)
      throw Error(R::ErrorCode::Rejected,
                  "the remote worker restarted after the job started, so its "
                  "Card secrets can no longer reach it");
    R::CardSecretsT secrets;
    secrets.secrets = std::move(body.secrets);
    auto bytes = encode(reply(std::move(secrets)));
    auto frame = length_prefix(static_cast<uint32_t>(bytes.size())) + bytes;
    clear_secret(bytes);
    size_t written = 0;
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (written < frame.size()) {
      const auto remaining = deadline - Clock::now();
      if (remaining <= Clock::duration::zero()) break;
      pollfd ready[]{{channel->value, POLLOUT, 0}, {shutdown_fd_, POLLIN, 0}};
      const auto timeout =
          std::chrono::ceil<std::chrono::milliseconds>(remaining);
      const int polled = ::poll(ready, 2, static_cast<int>(timeout.count()));
      if (polled < 0 && errno == EINTR) continue;
      if (polled <= 0 || ready[1].revents || !(ready[0].revents & POLLOUT))
        break;
      const auto n = ::write(channel->value, frame.data() + written,
                             frame.size() - written);
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (n <= 0) break;
      written += static_cast<size_t>(n);
    }
    const bool complete = written == frame.size();
    clear_secret(frame);
    if (!complete)
      throw Error(R::ErrorCode::Rejected,
                  "the job's invocation no longer reads its Card secrets");
  }
  auto spawn(Journal& p, const Sha256Hex& job, int lock) -> pid_t {
    decltype(cache_settings_)::node_type configuration;
    {
      std::lock_guard guard(channel_mutex_);
      configuration = cache_settings_.extract(channel_key(p, job));
    }
    if (configuration.empty()) return -1;
    auto& config = configuration.mapped();
    // The worker's own pull caches follow the host's, for jobs that start
    // from now on. Nix parallelism is the one this listener started with.
    for (const auto& cache : settings_.saved().pull_caches) {
      auto& pulls = config.settings->pull;
      if (std::ranges::any_of(pulls, [&](const auto& pull) {
            return pull->endpoint == cache->url;
          }))
        continue;
      auto pull = std::make_unique<R::CachePullSettingsT>();
      pull->endpoint = cache->url;
      pull->public_key = cache->public_key;
      pulls.push_back(std::move(pull));
    }
    const auto& own = settings_.started();
    const auto setting = [](uint32_t value) {
      return value ? std::to_string(value) : std::string();
    };
    Fd cache_file(::memfd_create("tsugumi-cache-settings",
                                 MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (cache_file.value < 0) return -1;
    auto bytes = encode(reply(std::move(*config.settings)));
    const auto write_settings = [&] {
      size_t offset = 0;
      while (offset < bytes.size()) {
        const auto written = ::write(cache_file.value, bytes.data() + offset,
                                     bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
      }
      return ::lseek(cache_file.value, 0, SEEK_SET) == 0 &&
             ::fcntl(
                 cache_file.value, F_ADD_SEALS,
                 F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0;
    };
    const auto cache_written = write_settings();
    clear_secret(bytes);
    if (!cache_written) return -1;
    Fd cache_adapter(::fcntl(cache_file.value, F_DUPFD_CLOEXEC, 10));
    if (cache_adapter.value < 0) return -1;
    Fd relay_adapter;
    if (config.relay.value >= 0) {
      relay_adapter.value = ::fcntl(config.relay.value, F_DUPFD_CLOEXEC, 10);
      if (relay_adapter.value < 0) return -1;
    }
    // A job with Card secrets gets a pipe whose write end only this listener
    // holds; its invocation reads the values from it just before the Card's
    // process starts.
    std::shared_ptr<Fd> channel;
    Fd channel_reader;
    try {
      if (!parse_spec(p.spec(job))->secrets.empty()) {
        int ends[2];
        if (::pipe2(ends, O_CLOEXEC) != 0) return -1;
        Fd writer(ends[1]);
        Fd reader(ends[0]);
        if (::fcntl(writer.value, F_SETFL, O_NONBLOCK) != 0) return -1;
        channel = std::make_shared<Fd>(std::move(writer));
        // Above the descriptors the supervisor's file actions assign.
        channel_reader.value = ::fcntl(reader.value, F_DUPFD_CLOEXEC, 10);
        if (channel_reader.value < 0) return -1;
      }
    } catch (const std::exception&) {
      return -1;
    }
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
      return -1;
    Fd listener_display(pair[0]), job_display(pair[1]);
    Fd adapter_display(::fcntl(job_display.value, F_DUPFD_CLOEXEC, 10));
    if (adapter_display.value < 0) return -1;
    std::shared_ptr<DisplayChannel> display;
    try {
      display = std::make_shared<DisplayChannel>(std::move(listener_display));
    } catch (const std::exception&) {
      return -1;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, lock, 5);
    posix_spawn_file_actions_adddup2(&actions, cache_adapter.value,
                                     kCacheSettingsFd);
    if (relay_adapter.value >= 0)
      posix_spawn_file_actions_adddup2(&actions, relay_adapter.value,
                                       kCacheRelayFd);
    posix_spawn_file_actions_adddup2(&actions, adapter_display.value,
                                     kDisplayChannelFd);
    if (channel)
      posix_spawn_file_actions_adddup2(&actions, channel_reader.value,
                                       kSecretChannelFd);
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
                                  std::string(job.view()),
                                  "5",
                                  (config_.root / "store").string(),
                                  config_.cgroup.string(),
                                  setting(own.nix_max_jobs),
                                  setting(own.nix_cores)};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc =
        ::posix_spawn(&pid, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc == 0) {
      std::lock_guard guard(channel_mutex_);
      const auto key = channel_key(p, job);
      if (channel) channels_[key] = std::move(channel);
      std::swap(displays_[key], display);
    }
    return rc == 0 ? pid : -1;
  }
  Config config_;
  std::string server_;
  std::string revision_;
  WorkerSettings& settings_;
  std::string system_;
  Dashboard dashboard_;
  std::string dashboard_sha256_;
  uint16_t dashboard_port_ = 0;
  std::atomic<Restart> restart_{Restart::None};
  std::atomic<uint32_t> running_jobs_{0};
  std::mutex seen_mutex_;
  std::map<std::string, std::chrono::system_clock::time_point> seen_;
  int shutdown_fd_;
  Fd journal_events_;
  std::map<Job, Fd> running_;
  std::set<Job> handoffs_;
  std::map<Job, stdfs::file_time_type> queued_;
  const std::string probe_life_ = crypto::random_token(32);
  std::atomic<uint32_t> active_jobs_{0};
  std::mutex project_mutex_;
  std::map<Sha256Hex, std::shared_ptr<Journal>> projects_;
  std::map<int, std::shared_ptr<Journal>> journal_watches_;
  std::mutex channel_mutex_;
  // Write ends of the Card secret pipes of running jobs, by journal and job.
  std::map<std::string, std::shared_ptr<Fd>> channels_;
  std::map<std::string, std::shared_ptr<DisplayChannel>> displays_;
  struct PendingCache {
    std::unique_ptr<R::CacheSettingsT> settings;
    Fd relay;
  };
  std::map<std::string, PendingCache> cache_settings_;
};

auto supervise(const std::string& self, const stdfs::path& root,
               const Sha256Hex& job, int lockfd, const stdfs::path& store,
               const stdfs::path& cgroup_root, const std::string& nix_max_jobs,
               const std::string& nix_cores) -> int {
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
  const auto directory = journal.job_path(job);
  const auto work = directory / "work";
  private_directory(work);
  const auto group = cgroup_path(cgroup_root, journal, job);
  pid_t child = -1;
  R::JobState outcome = R::JobState::Failed;
  std::string result, diagnostic;
  try {
    // Publish the identity only after Cancel can safely signal us. A Cancel
    // recorded before this point is picked up by the journal check below.
    sigset_t wake, inherited;
    sigemptyset(&wake);
    sigaddset(&wake, SIGUSR1);
    sigaddset(&wake, SIGCHLD);
    if (::sigprocmask(SIG_BLOCK, &wake, &inherited) != 0)
      throw std::runtime_error("cannot block supervisor signals");
    journal.supervisor_identity(job, static_cast<uint64_t>(::getpid()),
                                process_start(::getpid()), boot());
    if (journal.status(job).state == R::JobState::Cancelling)
      throw std::runtime_error("cancelled");
    const auto bytes = journal.spec(job);
    const auto spec = parse_spec(bytes);
    // The listener's pipe for the job's Card secrets; the supervisor only
    // passes it on to the invocation and never reads it.
    if (!spec->secrets.empty() && ::fcntl(kSecretChannelFd, F_GETFD) < 0)
      throw std::runtime_error("the job's Card secret channel is missing");
    fs::write_atomic(work / "spec.fb", bytes, 0600);
    Fd adapter(::open((work / "adapter.log").c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (adapter.value < 0) throw std::runtime_error("cannot open adapter log");
    const pid_t parent = ::getpid();
    child = ::fork();
    if (child < 0) throw std::runtime_error("cannot start isolated worker");
    if (child == 0) {
      ::sigprocmask(SIG_SETMASK, &inherited, nullptr);
      if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent)
        ::_exit(126);
      ::dup2(adapter.value, 1);
      ::dup2(adapter.value, 2);
      const auto blobs = directory / "blobs";
      ::execlp("unshare", "unshare", "--map-current-user", "--pid", "--fork",
               "--kill-child=SIGKILL", "--mount-proc", "--", self.c_str(),
               "--invoke", work.c_str(), blobs.c_str(), store.c_str(),
               group.c_str(), nix_max_jobs.c_str(), nix_cores.c_str(), nullptr);
      ::_exit(127);
    }
    if (!spec->secrets.empty()) ::close(kSecretChannelFd);
    ::close(kCacheSettingsFd);
    ::close(kCacheRelayFd);
    ::close(kDisplayChannelFd);
    int status = 0;
    for (;;) {
      const auto current = journal.status(job);
      if (current.state == R::JobState::Cancelling)
        throw std::runtime_error("cancelled");
      const auto done = ::waitpid(child, &status, WNOHANG);
      if (done == child) {
        child = -1;
        break;
      }
      if (done < 0 && errno != EINTR)
        throw std::runtime_error("cannot wait for worker");
      if (::sigwaitinfo(&wake, nullptr) < 0 && errno != EINTR)
        throw std::runtime_error("cannot wait for supervisor event");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      const auto size = stdfs::file_size(work / "adapter.log");
      diagnostic = fs::read_range(work / "adapter.log",
                                  size > 2048 ? size - 2048 : 0, 2048)
                       .value_or("");
      throw std::runtime_error("worker failed: " + diagnostic);
    }
    result = journal.import_results(job);
    outcome = R::JobState::Succeeded;
  } catch (const std::exception& e) {
    diagnostic = e.what();
  }
  if (child > 0) {
    ::kill(child, SIGKILL);
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
  }
  remove_card_cgroup(group);
  // Keep partial outputs and logs as well as imported blobs until release.
  // Stopping a supervisor frees its execution slot without deleting run data.
  journal.finish(job, outcome, result, diagnostic);
  return 0;
}

auto unix_listener(const stdfs::path& path) -> int {
  if (!path.is_absolute())
    throw std::runtime_error("Unix socket path must be absolute: " +
                             path.string());
  const auto address = unix_socket::Address(path.string());
  // The singleton lock owns this private endpoint; never unlink arbitrary
  // files.
  struct stat st{};
  if (::lstat(path.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid())
      throw std::runtime_error("Unix endpoint is not an owned socket");
    ::unlink(path.c_str());
  }
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 ||
      ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) !=
          0 ||
      ::listen(fd, 64) != 0)
    throw std::runtime_error("cannot listen on Unix socket");
  ::chmod(path.c_str(), 0600);
  return fd;
}
// Binds `address` (an IP literal) and `port`, 0 for any free port, which is
// replaced with the bound one.
auto tcp_listener(const std::string& address, uint16_t& port) -> int {
  const bool v6 = address.find(':') != std::string::npos;
  int fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  sockaddr_in6 b{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  const bool ipv4 = ::inet_pton(AF_INET, address.c_str(), &a.sin_addr) == 1;
  b.sin6_family = AF_INET6;
  b.sin6_port = htons(port);
  const bool ipv6 = ::inet_pton(AF_INET6, address.c_str(), &b.sin6_addr) == 1;
  if (!(v6 ? ipv6 : ipv4))
    throw std::runtime_error("listen_address must be an IP address");
  if (fd < 0 ||
      ::bind(fd,
             v6 ? reinterpret_cast<sockaddr*>(&b)
                : reinterpret_cast<sockaddr*>(&a),
             v6 ? sizeof b : sizeof a) != 0 ||
      ::listen(fd, 64) != 0)
    throw std::runtime_error("cannot listen on " + address + " port " +
                             std::to_string(port) + ": " +
                             std::strerror(errno));
  sockaddr_storage bound{};
  socklen_t size = sizeof bound;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &size) != 0)
    throw std::runtime_error("cannot read the listener's port");
  port = ntohs(v6 ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port
                  : reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
  return fd;
}
// Reads the new administrator password: twice without echo from a terminal,
// otherwise one line from standard input.
auto read_password() -> std::string {
  const auto line = [] {
    std::string text;
    if (!std::getline(std::cin, text))
      throw std::runtime_error("no password was given");
    if (!text.empty() && text.back() == '\r') text.pop_back();
    return text;
  };
  if (!::isatty(STDIN_FILENO)) return line();
  termios saved{};
  if (::tcgetattr(STDIN_FILENO, &saved) != 0)
    throw std::runtime_error("cannot read the terminal settings");
  auto quiet = saved;
  quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
  struct Restore {
    termios value;
    ~Restore() { ::tcsetattr(STDIN_FILENO, TCSANOW, &value); }
  } restore{saved};
  ::tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
  std::cerr << "New administrator password: " << std::flush;
  auto first = line();
  std::cerr << "\nRepeat it: " << std::flush;
  const auto second = line();
  std::cerr << '\n';
  if (first != second) throw std::runtime_error("the passwords differ");
  return first;
}
auto admin_password_main(stdfs::path root, std::span<char*> args) -> int {
  for (size_t i = 0; i < args.size(); ++i) {
    if (std::string_view(args[i]) == "--state-dir" && i + 1 < args.size())
      root = args[++i];
    else
      throw std::runtime_error(
          "usage: tsugumi-workerd admin-password [--state-dir DIR]");
  }
  if (!root.is_absolute())
    throw std::runtime_error("the state directory must be an absolute path");
  private_directory(root);
  const auto chosen = read_password();
  if (chosen.size() < 8 || chosen.size() > 1024)
    throw std::runtime_error("use a password of 8 to 1024 bytes");
  fs::write_atomic(root / "admin-password",
                   tsugumi::password::Hash(chosen) + "\n", 0600);
  std::cout << "The administrator password is set. Log in on the dashboard.\n";
  return 0;
}
// Returns true when the dashboard asked the listener to start again.
auto run(Config config) -> bool {
  private_directory(config.root);
  Fd singleton(::open((config.root / "worker.lock").c_str(),
                      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (singleton.value < 0 || ::flock(singleton.value, LOCK_EX | LOCK_NB) != 0)
    throw std::runtime_error(
        "another tsugumi-workerd is already running with the state directory " +
        config.root.string() + "; run one worker per user");
  if (config.unix_socket.empty())
    config.unix_socket = config.root / "worker.sock";
  peer_tls::Identity identity(config.root / "identity.pem");
  if (config.print_key) {
    std::cout << identity.public_key() << '\n';
    return false;
  }
  WorkerSettings settings(config.root);
  const auto& started = settings.started();
  const auto authorized_keys = config.root / "authorized_keys";
  if (!stdfs::exists(authorized_keys))
    fs::write_atomic(authorized_keys, "# One ed25519: SPKI key per line.\n",
                     0600);
  peer_tls::Context tls(identity, true, [&](std::string_view key) {
    return peer_tls::Authorized(authorized_keys, key);
  });
  // Browsers refuse Ed25519 server certificates, so the dashboard has a
  // P-256 certificate of its own. Hosts learn its hash over the pinned link.
  peer_tls::WebIdentity web_identity(config.root / "dashboard-identity.pem");
  peer_tls::Context web(web_identity);
  int shutdown_pipe[2];
  if (::pipe2(shutdown_pipe, O_CLOEXEC) != 0)
    throw std::runtime_error("cannot create shutdown pipe");
  Fd shutdown_reader(shutdown_pipe[0]), shutdown_writer(shutdown_pipe[1]);
  Daemon daemon(config, settings, web_identity.certificate_sha256(),
                shutdown_reader.value);
  Fd unixfd(unix_listener(config.unix_socket));
  uint16_t port = started.port, dashboard_port = started.dashboard_port;
  Fd tcpfd(tcp_listener(started.listen_address, port));
  Fd webfd(tcp_listener(started.listen_address, dashboard_port));
  daemon.listening(port, dashboard_port);
  ::signal(SIGTERM, stop);
  ::signal(SIGINT, stop);
  ::signal(SIGPIPE, SIG_IGN);
  daemon.recover();
  Tsugumi::RemoteWorker::WorkerReadyT ready;
  ready.port = port;
  ready.pid = static_cast<uint64_t>(::getpid());
  ready.dashboard_port = dashboard_port;
  fs::write_atomic(config.root / "worker.ready",
                   flat::encode<Tsugumi::RemoteWorker::WorkerReady>(ready),
                   0600);
  std::atomic<size_t> active{0};
  struct Client {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<Client> clients;
  const auto host = started.listen_address.find(':') == std::string::npos
                        ? started.listen_address
                        : "[" + started.listen_address + "]";
  std::string fingerprint;
  for (const auto c : web_identity.certificate_sha256()) {
    if (!fingerprint.empty() && fingerprint.size() % 3 == 2) fingerprint += ':';
    fingerprint += static_cast<char>(std::toupper(c));
  }
  log("public key: " + identity.public_key());
  const auto revision = build_revision();
  log("worker contract " + std::to_string(kContract) + ", revision " +
      (revision.empty() ? std::string("unknown") : revision));
  log("listening with TLS 1.3 on unix:" + config.unix_socket.string() +
      " and tls://" + host + ":" + std::to_string(port));
  log("dashboard: https://" + host + ":" + std::to_string(dashboard_port) +
      "/ (certificate SHA-256 " + fingerprint + ")");
  bool restarting = false;
  while (!stopping && !(restarting = daemon.restart_due())) {
    for (auto i = clients.begin(); i != clients.end();) {
      if (i->done->load()) {
        i->thread.join();
        i = clients.erase(i);
      } else
        ++i;
    }
    std::vector<pollfd> fds{{unixfd.value, POLLIN, 0},
                            {tcpfd.value, POLLIN, 0},
                            {webfd.value, POLLIN, 0}};
    daemon.watch_events(fds);
    if (::poll(fds.data(), fds.size(), 100) < 0) continue;
    const auto descriptors = std::span(fds);
    daemon.tick(descriptors.subspan(3));
    for (const auto& listener : descriptors.first(3))
      if (listener.revents & POLLIN) {
        const bool page = listener.fd == webfd.value;
        const int fd = ::accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        if (active.load() >= kConnections) {
          ::close(fd);
          continue;
        }
        ++active;
        auto done = std::make_shared<std::atomic<bool>>(false);
        clients.push_back({std::thread([&, fd, done, page] {
                             Fd connection(fd);
                             try {
                               peer_tls::ConfigureSocket(fd, 30);
                               peer_tls::Stream stream(page ? web : tls, fd,
                                                       true);
                               if (page)
                                 daemon.serve_dashboard(stream);
                               else
                                 daemon.serve_stream(stream);
                             } catch (const std::exception&) {
                             }
                             --active;
                             done->store(true);
                           }),
                           done});
      }
  }
  // Closing the writer wakes every in-flight secret delivery.
  ::close(std::exchange(shutdown_writer.value, -1));
  for (auto& client : clients) client.thread.join();
  ::unlink(config.unix_socket.c_str());
  // Detached supervisors and their namespaces continue through listener
  // restarts.
  if (restarting) log("restarting as the dashboard asked");
  return restarting;
}
}  // namespace
}  // namespace tsugumi::workerd

auto remote_workerd_main(int argc, char** argv) -> int {
  using namespace tsugumi::workerd;
  ::umask(0077);
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--check-namespace")
      return ::getpid() == 1 ? 0 : 1;
    if (argc == 8 && std::string_view(argv[1]) == "--invoke") {
      invoke(argv[2], argv[3], argv[4], argv[5], {argv[6], argv[7]});
      return 0;
    }
    if (argc >= 2 && std::string_view(argv[1]) == "jobs")
      return jobs_main(default_root(),
                       std::span(argv + 2, static_cast<size_t>(argc - 2)));
    if (argc >= 2 && std::string_view(argv[1]) == "admin-password")
      return admin_password_main(
          default_root(), std::span(argv + 2, static_cast<size_t>(argc - 2)));
    const auto self = stdfs::canonical("/proc/self/exe").string();
    if (argc == 9 && std::string_view(argv[1]) == "--supervise")
      return supervise(self, argv[2], require_sha256_hex(argv[3], "job ID"),
                       std::stoi(argv[4]), argv[5], argv[6], argv[7], argv[8]);
    Config c;
    c.root = default_root();
    c.executable = self;
    c.arguments.assign(argv, argv + argc);
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
      else if (option == "--cgroup-root")
        c.cgroup = value;
      else
        throw std::runtime_error(
            "unknown remote worker option " + std::string(option) +
            "; listen address, ports and concurrency are in the state "
            "directory's settings.json");
    }
    if (!c.root.is_absolute())
      throw std::runtime_error("the state directory must be an absolute path");
    auto arguments = c.arguments;
    if (!run(std::move(c))) return 0;
    // The dashboard's restart: the same command, so the new listener reads
    // settings.json again and adopts the running supervisors.
    std::vector<char*> exec;
    for (auto& argument : arguments) exec.push_back(argument.data());
    exec.push_back(nullptr);
    ::execv(self.c_str(), exec.data());
    throw std::runtime_error("cannot start the listener again: " +
                             std::string(std::strerror(errno)));
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
