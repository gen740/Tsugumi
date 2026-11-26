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
#include <span>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "display_channel.hh"
#include "journal.hh"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/rpc/peer_tls.hh"
#include "tsugumi/rpc/unix_socket.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/invocation.hh"
#include "tsugumi/workerd/worker_proc.hh"

extern char** environ;
namespace tsugumi::workerd {
auto invoke(const stdfs::path& directory, const stdfs::path& blobs,
            const stdfs::path& store, const stdfs::path& group) -> void;
auto jobs_main(stdfs::path root, std::span<char*> args) -> int;
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
auto notify_cancel(const Record& state) -> void {
  if (state.pid <= 1 || state.boot != boot()) return;
  Fd pidfd(static_cast<int>(::syscall(SYS_pidfd_open, state.pid, 0)));
  if (pidfd.value < 0) return;
  try {
    if (process_start(static_cast<pid_t>(state.pid)) == state.start)
      if (::syscall(SYS_pidfd_send_signal, pidfd.value, SIGUSR1, nullptr, 0) !=
              0 &&
          errno != ESRCH)
        log("cannot notify execution supervisor: " +
            std::string(std::strerror(errno)));
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
auto supervisor_lock(const Journal& journal, const std::string& job) -> int {
  return ::open(journal.supervisor_lock_path(job).c_str(),
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
    c.active_jobs = active_jobs_.load();
    c.probe_life = probe_life_;
    c.worker_contract = kContract;
    return c;
  }
  auto project(const std::string& scope) -> std::shared_ptr<Journal> {
    std::lock_guard guard(project_mutex_);
    auto& entry = projects_[scope];
    if (!entry)
      entry = std::make_shared<Journal>(config_.root / "projects" / scope);
    return entry;
  }
  auto handle(R::RequestT& q) -> R::MessageT {
    if (q.body.type == R::RequestBody::NONE ||
        q.body.type == R::RequestBody::CatalogRequest)
      throw Error(R::ErrorCode::Invalid, "invalid remote request");
    if (q.project_scope.empty()) {
      if (q.body.type != R::RequestBody::ProbeRequest || !q.instance_id.empty())
        throw Error(R::ErrorCode::Rejected,
                    "host-authorized project scope required");
      return reply(capabilities());
    }
    const auto journal = project(q.project_scope);
    if ((!q.instance_id.empty() && q.instance_id != journal->instance()) ||
        (q.body.type != R::RequestBody::ProbeRequest && q.instance_id.empty()))
      throw Error(R::ErrorCode::InstanceChanged,
                  "project journal identity changed");
    if (q.body.type == R::RequestBody::ProbeRequest) {
      return reply(capabilities(journal->instance()));
    }
    const auto with_log_status = [&](R::JobStatusT status) {
      std::error_code ec;
      status.log_truncated = std::filesystem::is_regular_file(
          journal->job_path(q.job_id) / "work/log-truncated", ec);
      return status;
    };
    const auto log_slice = [&](uint64_t offset, uint32_t limit) {
      auto range = journal->log(
          q.job_id, offset, std::min(limit, kLogChunkBytes), q.core_life_id);
      return [offset, range = std::move(range)](R::JobStatusT status) {
        status.log_offset = offset;
        status.log_size = range.first;
        status.log_bytes.assign(range.second.begin(), range.second.end());
        return status;
      };
    };
    switch (q.body.type) {
      case R::RequestBody::PutBlobRequest:
        journal->put(q.job_id, *q.body.AsPutBlobRequest(), q.core_life_id);
        return reply(R::AckT{});
      case R::RequestBody::GetBlobRequest:
        return reply(
            journal->get(q.job_id, *q.body.AsGetBlobRequest(), q.core_life_id));
      case R::RequestBody::SubmitRequest: {
        const auto& bytes = q.body.AsSubmitRequest()->spec;
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
        return reply(
            journal->submit(q.job_id, data, *spec, q.core_life_id).status());
      }
      case R::RequestBody::ViewSendRequest: {
        const auto record = journal->status(q.job_id, q.core_life_id);
        const auto channel = display_channel(*journal, q.job_id);
        if (record.state != R::JobState::Running || !channel ||
            !channel->send(*q.body.AsViewSendRequest()))
          throw Error(R::ErrorCode::Rejected, "the Card cannot accept a view message");
        return reply(R::AckT{});
      }
      case R::RequestBody::StatusRequest: {
        auto status =
            with_log_status(journal->status(q.job_id, q.core_life_id).status());
        const auto& query = *q.body.AsStatusRequest();
        status =
            log_slice(query.log_offset, query.log_limit)(std::move(status));
        if (status.state == R::JobState::Running) {
          std::error_code ec;
          status.secrets_wanted = stdfs::exists(
              journal->job_path(q.job_id) / "work" / kSecretsWanted, ec);
          if (!stdfs::exists(
                  journal->job_path(q.job_id) / "work/artifact-reply", ec))
            status.artifact_wanted = fs::read_file(journal->job_path(q.job_id) /
                                                   "work/artifact-wanted")
                                         .value_or("");
          const auto bytes =
              fs::read_file(journal->job_path(q.job_id) / "work/progress");
          if (const auto progress =
                  bytes ? remote::decode<R::TransferProgress>(*bytes)
                        : std::nullopt) {
            status.transfer_name = progress->name;
            status.transfer_done = progress->done;
            status.transfer_total = progress->total;
          }
        }
        if (const auto channel = display_channel(*journal, q.job_id))
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
        const auto state = journal->cancel(q.job_id, query.fingerprint);
        if (state.state == R::JobState::Cancelling) notify_cancel(state);
        return reply(slice(with_log_status(state.status())));
      }
      case R::RequestBody::ReleaseRequest:
        journal->release(q.job_id, q.body.AsReleaseRequest()->fingerprint);
        {
          std::lock_guard guard(channel_mutex_);
          displays_.erase(channel_key(*journal, q.job_id));
        }
        return reply(R::AckT{});
      case R::RequestBody::JobArtifactRequest:
        journal->deliver_artifact(q.job_id, *q.body.AsJobArtifactRequest(),
                                  q.core_life_id);
        return reply(R::AckT{});
      case R::RequestBody::JobSecretsRequest:
        deliver_secrets(*journal, q.job_id, q.core_life_id,
                        *q.body.AsJobSecretsRequest());
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
      auto bytes = stream.Read(n);
      auto m = decode(bytes);
      clear_secret(bytes);
      auto* q = m->payload.AsRequest();
      if (!q) throw Error(R::ErrorCode::Invalid, "expected a request");
      auto response = encode(handle(*q));
      stream.Write(length_prefix(static_cast<uint32_t>(response.size())) +
                   response);
    } catch (const Error& e) {
      auto bytes = encode(failure(e.code, e.what()));
      stream.Write(length_prefix(static_cast<uint32_t>(bytes.size())) + bytes);
    }
  }
  auto tick() -> void {
    {
      std::lock_guard guard(channel_mutex_);
      std::erase_if(displays_,
                    [](const auto& item) { return item.second->expired(); });
    }
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
            continue;
          }
          // No supervisor runs the job, so no invocation reads its channel.
          close_channel(*p, job);
          if (state.state == R::JobState::Running ||
              state.state == R::JobState::Cancelling) {
            remove_card_cgroup(cgroup_path(config_.cgroup, *p, job));
            p->finish(job, R::JobState::Interrupted, {},
                      "execution supervisor ended without recording a result");
          }
          if (terminal(state.state)) {
            remove_card_cgroup(cgroup_path(config_.cgroup, *p, job));
          }
          p->clean_released(job);
          if (p->status(job).state == R::JobState::Queued)
            pending.emplace_back(p, job);
        } catch (const std::exception& e) {
          ++active;
          log(e.what());
        }
      }
    // Read each submission time once: `tsugumi-workerd jobs remove` can erase
    // a queued job's spec meanwhile, and its claim then finds the tombstone.
    std::vector<std::pair<stdfs::file_time_type, size_t>> order;
    for (size_t i = 0; i < pending.size(); ++i) {
      std::error_code ec;
      order.emplace_back(
          stdfs::last_write_time(
              pending[i].first->job_path(pending[i].second) / "spec.fb", ec),
          i);
    }
    std::ranges::sort(order);
    decltype(pending) sorted;
    for (const auto& [_, i] : order) sorted.push_back(std::move(pending[i]));
    pending = std::move(sorted);
    active_jobs_.store(static_cast<uint32_t>(active + pending.size()));
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
      else {
        close_channel(*p, job);
        p->finish(job, R::JobState::Failed, {}, "supervisor launch failed");
      }
    }
  }

 private:
  static auto channel_key(const Journal& journal, const std::string& job)
      -> std::string {
    return journal.root().string() + '\n' + job;
  }
  auto display_channel(const Journal& journal, const std::string& job)
      -> std::shared_ptr<DisplayChannel> {
    std::lock_guard guard(channel_mutex_);
    const auto found = displays_.find(channel_key(journal, job));
    return found == displays_.end() ? nullptr : found->second;
  }
  auto close_channel(const Journal& journal, const std::string& job) -> void {
    std::lock_guard guard(channel_mutex_);
    channels_.erase(channel_key(journal, job));
  }
  // Writes a running job's Card secrets into the pipe its invocation reads,
  // once, and closes it. Only this listener life holds the pipe: after a
  // restart the invocation reads end of file and fails, and nothing resends.
  auto deliver_secrets(const Journal& journal, const std::string& job,
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
    while (written < frame.size()) {
      pollfd ready{channel->value, POLLOUT, 0};
      if (::poll(&ready, 1, 10000) <= 0) break;
      const auto n = ::write(channel->value, frame.data() + written,
                             frame.size() - written);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      written += static_cast<size_t>(n);
    }
    const bool complete = written == frame.size();
    clear_secret(frame);
    if (!complete)
      throw Error(R::ErrorCode::Rejected,
                  "the job's invocation no longer reads its Card secrets");
  }
  auto spawn(Journal& p, const std::string& job, int lock) -> bool {
    // A job with Card secrets gets a pipe whose write end only this listener
    // holds; its invocation reads the values from it just before the Card's
    // process starts.
    std::shared_ptr<Fd> channel;
    Fd channel_reader;
    try {
      if (!parse_spec(p.spec(job))->secrets.empty()) {
        int ends[2];
        if (::pipe2(ends, O_CLOEXEC) != 0) return false;
        channel = std::make_shared<Fd>(ends[1]);
        // Above the descriptors the supervisor's file actions assign.
        channel_reader.value = ::fcntl(ends[0], F_DUPFD_CLOEXEC, 10);
        ::close(ends[0]);
        if (channel_reader.value < 0) return false;
      }
    } catch (const std::exception&) {
      return false;
    }
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
      return false;
    Fd listener_display(pair[0]), job_display(pair[1]);
    Fd adapter_display(::fcntl(job_display.value, F_DUPFD_CLOEXEC, 10));
    if (adapter_display.value < 0) return false;
    std::shared_ptr<DisplayChannel> display;
    try {
      display = std::make_shared<DisplayChannel>(std::move(listener_display));
    } catch (const std::exception&) {
      return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, lock, 5);
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
                                  job,
                                  "5",
                                  (config_.root / "store").string(),
                                  config_.cgroup.string()};
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
      displays_[key] = std::move(display);
    }
    return rc == 0;
  }
  Config config_;
  std::string server_, system_;
  const std::string probe_life_ = crypto::random_token(32);
  std::atomic<uint32_t> active_jobs_{0};
  std::mutex project_mutex_;
  std::map<std::string, std::shared_ptr<Journal>> projects_;
  std::mutex channel_mutex_;
  // Write ends of the Card secret pipes of running jobs, by journal and job.
  std::map<std::string, std::shared_ptr<Fd>> channels_;
  std::map<std::string, std::shared_ptr<DisplayChannel>> displays_;
};

auto supervise(const std::string& self, const stdfs::path& root,
               const std::string& job, int lockfd, const stdfs::path& store,
               const stdfs::path& cgroup_root) -> int {
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
               group.c_str(), nullptr);
      ::_exit(127);
    }
    if (!spec->secrets.empty()) ::close(kSecretChannelFd);
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
auto tcp_listener(Config& config) -> int {
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
  sockaddr_storage bound{};
  socklen_t size = sizeof bound;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &size) != 0)
    throw std::runtime_error("cannot read TLS listener port");
  config.port = ntohs(v6 ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port
                         : reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
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
  Tsugumi::RemoteWorker::WorkerReadyT ready;
  ready.port = config.port;
  ready.pid = static_cast<uint64_t>(::getpid());
  fs::write_atomic(config.root / "worker.ready",
                   flat::encode<Tsugumi::RemoteWorker::WorkerReady>(ready),
                   0600);
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
                               peer_tls::ConfigureSocket(fd);
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

auto remote_workerd_main(int argc, char** argv) -> int {
  using namespace tsugumi::workerd;
  ::umask(0077);
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--check-namespace")
      return ::getpid() == 1 ? 0 : 1;
    if (argc == 6 && std::string_view(argv[1]) == "--invoke") {
      invoke(argv[2], argv[3], argv[4], argv[5]);
      return 0;
    }
    if (argc >= 2 && std::string_view(argv[1]) == "jobs")
      return jobs_main(default_root(),
                       std::span(argv + 2, static_cast<size_t>(argc - 2)));
    const auto self = stdfs::canonical("/proc/self/exe").string();
    if (argc == 7 && std::string_view(argv[1]) == "--supervise")
      return supervise(self, argv[2], argv[3], std::stoi(argv[4]), argv[5],
                       argv[6]);
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
        if (n > 65535) throw std::runtime_error("invalid port");
        c.port = static_cast<uint16_t>(n);
      } else if (option == "--max-jobs") {
        const auto n = std::stoul(value);
        if (!n || n > 65536) throw std::runtime_error("invalid max-jobs");
        c.max_jobs = static_cast<uint32_t>(n);
      } else if (option == "--cgroup-root")
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
