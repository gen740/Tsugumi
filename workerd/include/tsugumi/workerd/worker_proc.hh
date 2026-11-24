// Owner side of one tsugumi-worker process.
#pragma once

#include <sys/types.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "tsugumi/rpc/trace.hh"
#include "tsugumi/util/text.hh"
#include "tsugumi/workerd/values.hh"

namespace tsugumi {

struct WorkerSecrets {
  std::vector<std::pair<std::string, std::string>> values;
  ~WorkerSecrets();
};

struct WorkerSpawn {
  std::string worker_binary;
  std::string store_path = "/nix/store";
  std::string package;
  std::vector<std::string> closure;
  // Remote supervision knows this leaf and cleans it on interruption.
  std::filesystem::path cgroup;
  bool shared_vm = false;
  std::shared_ptr<WorkerSecrets> secrets;
  // The worker's work/ and tmp/ live with the process.
  // Local workerd retains process ownership while the core reads the channel.
  bool channels_borrowed = false;
  std::string scratch;
  SandboxOptions sandbox;
  // Fixed read-only runtime paths.
  std::vector<std::string> ro_paths;
  // Linux mounts this at kSandboxArtifactsRoot. Seatbelt allows it in place.
  std::string artifacts_root;
  // Covered with an empty tmpfs for sandbox: false Cards (the control folder
  // with the RPC token, tsugumid's state); ro paths and the scratch folder
  // inside them are bound again on top.
  std::vector<std::string> hidden_paths;
  // Per Card: outside network (otherwise the worker has its
  // own network namespace with nothing in it) and folders of the Mac.
  bool network = false;
  bool gui = false;
  struct Bind {
    std::string source;
    std::string target;
    bool write = false;
    std::string name;
  };
  std::vector<Bind> binds;
  std::optional<uid_t> uid;
  std::optional<gid_t> gid;
};

inline constexpr auto kSandboxArtifactsRoot = "/tsugumi-artifacts";

// The opaque output envelope the worker reported.
struct RawOutput {
  std::string name;
  std::string bytes;
  std::vector<ArtifactKey> artifacts;
};

struct WorkerOutcome {
  std::vector<RawOutput> outputs;
  std::optional<FailureData> failure;
  std::vector<ArtifactKey> created = {};
};

// Whether a Card worker runs as its own uid on the host (folders it writes are
// then handed to that uid when tsugumid is root): not under bubblewrap, whose
// user namespace maps the Card's uid to tsugumid's own.
auto worker_has_bwrap() -> bool;
auto worker_limits_available() -> bool;
auto worker_gui_available() -> bool;
auto remove_card_cgroup(const std::filesystem::path& path) -> void;
bool worker_uid_on_host();

class WorkerProc {
  trace::Link trace_link_;

 public:
  using LogSink =
      std::function<void(LogStream stream, const std::string& text)>;
  using DisplayCredit = std::function<void()>;
  using DisplaySink = std::function<void(WorkerDisplayData, DisplayCredit)>;
  struct ViewReceiver {
    std::mutex mutex;
    WorkerProc* worker = nullptr;
    std::function<bool(const WorkerDisplayData&)> remote;
    auto send(const WorkerDisplayData& display, DisplayCredit complete = {})
        -> bool;
  };

  // Throws Failed(permission_denied) when the sandbox cannot be applied.
  explicit WorkerProc(const WorkerSpawn& spawn);
  // A borrowed connection has no process/scratch ownership. Workerd retains
  // that authority and performs cleanup and termination on request.
  struct Borrowed {
    std::array<int, 4> descriptors;
    std::string scratch;
    uint64_t call_id = 0;
    uint64_t lease = 0;
    std::function<void()> clear;
    std::function<void(bool)> terminate;
    std::function<void()> abandon;
  };
  explicit WorkerProc(Borrowed borrowed);
  ~WorkerProc();
  WorkerProc(const WorkerProc&) = delete;

  // Sends one call and waits for its result. On cancel or deadline the worker
  // gets WorkerCancel, then the process group is killed after one second.
  WorkerOutcome call(
      std::string frame, uint64_t call_id, const std::atomic<bool>* cancel,
      std::optional<std::chrono::steady_clock::time_point> deadline);
  uint64_t next_call_id() { return ++call_id_; }
  auto call_id() const -> uint64_t { return call_id_; }
  auto set_call_id(uint64_t value) -> void { call_id_ = value; }
  auto lease_id() const -> uint64_t { return lease_id_; }
  auto returning_lease() -> void { borrowed_abandon_ = {}; }
  auto hand_over() -> std::array<int, 4>;
  // Releasing a resident lease transfers log ownership to workerd and leaves
  // the core's data reader attached until the process exits.
  auto idle() -> void;
  auto resume(Borrowed borrowed) -> void;
  auto scratch() const -> const std::string& { return scratch_; }
  // The sink may run on the reader thread between invocations. It must own
  // its captures and return promptly.
  auto set_display(DisplaySink display) -> void;
  // Admits one reverse message; returns its credit after forwarding.
  auto send_display(const WorkerDisplayData& display,
                    DisplayCredit complete = {}) -> bool;
  auto view_receiver() -> std::weak_ptr<ViewReceiver>;
  auto resume_logs() -> void;
  // Where the worker's output goes while it serves the current call.
  void set_logs(LogSink logs);
  // The process still runs (checked without reaping it).
  bool alive() const;
  void terminate(bool force = false);
  std::string work_dir() const { return scratch_ + "/work"; }
  std::string tmp_dir() const { return scratch_ + "/tmp"; }
  // Empties the temporary directory between calls.
  void clear_scratch();
  // Resident memory of the worker process, when known.
  std::optional<uint64_t> rss_bytes() const;

 private:
  auto start_data_reader() -> void;
  auto stop_data_reader() -> void;
  auto read_data() -> void;
  auto write_display() -> void;
  auto return_display_credit(const std::string& view, size_t bytes) -> void;
  auto stop_log_reader() -> void;
  void read_logs(bool drain);
  void send(int fd, const std::string& frame, bool observe = true);

  pid_t pid_ = -1;
  int data_fd_ = -1;
  int control_fd_ = -1;
  int out_fd_ = -1;
  int err_fd_ = -1;
  bool alive_ = false;
  std::string scratch_;
  std::filesystem::path cgroup_;
  bool x11_mounted_ = false;
  // Darwin's permission repair runs with the same authority as the Card.
  std::function<void(int, const char*)> recover_permissions_;
  uint64_t call_id_ = 0;
  std::mutex data_mutex_;
  std::mutex control_mutex_;
  struct ControlView {
    std::deque<std::pair<std::string, DisplayCredit>> pending;
    size_t pending_bytes = 0;
    size_t reverse_credit = 0;
    size_t forward_credit = 0;
  };
  std::mutex display_mutex_;
  std::condition_variable display_ready_;
  std::map<std::string, ControlView> control_views_;
  std::thread display_writer_;
  std::condition_variable data_ready_;
  std::thread data_thread_;
  std::stop_source data_stop_;
  std::shared_ptr<ViewReceiver> view_receiver_;
  std::array<int, 2> data_wake_{-1, -1};
  uint64_t awaiting_ = 0;
  bool data_closed_ = false;
  std::optional<WorkerOutcome> pending_;
  std::shared_ptr<DisplaySink> display_;
  LogSink logs_;
  std::mutex log_mu_;
  text::Utf8Decoder out_dec_;
  text::Utf8Decoder err_dec_;
  std::thread log_thread_;
  std::array<int, 2> log_wake_{-1, -1};
  std::atomic<bool> stop_logs_{false};
  bool borrowed_ = false;
  uint64_t lease_id_ = 0;
  std::function<void()> borrowed_clear_;
  std::function<void(bool)> borrowed_terminate_;
  std::function<void()> borrowed_abandon_;
};

// Builds a WorkerMessage{call_id, WorkerCall} frame.
std::string worker_call_frame(
    uint64_t call_id, std::span<const Value> inputs,
    const std::vector<types::PortType>& outputs,
    std::optional<uint64_t> deadline_ms,
    const std::filesystem::path& artifact_directory = {},
    std::span<const ArtifactGrant> grants = {}, bool drain_display = false);

}  // namespace tsugumi
