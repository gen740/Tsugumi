// Owner side of one tsugumi-worker process.
#pragma once

#include "tsugumi/rpc/trace.hh"

#include <sys/types.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "tsugumi/util/text.hh"
#include "tsugumi/workerd/values.hh"

namespace tsugumi {

// Only the realised program enters worker ownership; evaluation, build keys
// and recorded environments remain with the caller.
struct WorkerProgram {
  std::optional<std::string> package;
  std::optional<std::string> bash;
  std::string pre_run;
  std::string post_run;
  std::string views;
};

struct WorkerSpawn {
  std::string worker_binary;
  std::string bash;
  std::string store_path = "/nix/store";
  WorkerProgram program;
  // The worker's own folder: work/, tmp/, out/, home/, inputs/, and
  // scripts/ for preRun / a bash run / postRun. Created with the worker
  // and removed when it ends, so it lives as long as the worker.
  std::string scratch;
  bool sandbox = true;
  // Fixed read-only runtime paths and, remotely, the job's staged inputs;
  // never project data roots. Local inputs use inputs_dir().
  std::vector<std::string> ro_paths;
  // The Card's key, used by the remote invocation launcher.
  std::string card;
  // Covered with an empty tmpfs for sandbox: false Cards (the control folder
  // with the RPC token, tsugumid's state); ro paths and the scratch folder
  // inside them are bound again on top.
  std::vector<std::string> hidden_paths;
  // Experimental, per Card: outside network (otherwise the worker has its
  // own network namespace with nothing in it) and folders of the Mac.
  bool network = false;
  struct Bind {
    std::string source;
    std::string target;
    bool write = false;
  };
  std::vector<Bind> binds;
  std::optional<uid_t> uid;
  std::optional<gid_t> gid;
};

struct WorkerOutcome {
  std::vector<std::pair<std::string, std::string>> outputs;
  std::optional<FailureData> failure;
};

// Whether a Card worker runs as its own uid on the host (folders it writes are
// then handed to that uid when tsugumid is root): not under bubblewrap, whose
// user namespace maps the Card's uid to tsugumid's own.
bool worker_uid_on_host();

class WorkerProc {
  trace::Link trace_link_;
 public:
  using LogSink =
      std::function<void(LogStream stream, const std::string& text)>;
  using DisplaySink = std::function<void(const DisplayData&)>;

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
      const std::string& frame, uint64_t call_id,
      const std::atomic<bool>* cancel,
      std::optional<std::chrono::steady_clock::time_point> deadline,
      const DisplaySink& display);
  uint64_t next_call_id() { return ++call_id_; }
  auto call_id() const -> uint64_t { return call_id_; }
  auto set_call_id(uint64_t value) -> void { call_id_ = value; }
  auto lease_id() const -> uint64_t { return lease_id_; }
  auto returning_lease() -> void { borrowed_abandon_ = {}; }
  auto hand_over() -> std::array<int, 4>;
  auto resume_logs() -> void;
  // Where the worker's output goes while it serves the current call.
  void set_logs(LogSink logs);
  // The process still runs (checked without reaping it).
  bool alive() const;
  void terminate(bool force = false);
  std::string work_dir() const { return scratch_ + "/work"; }
  std::string tmp_dir() const { return scratch_ + "/tmp"; }
  std::string out_dir() const { return scratch_ + "/out"; }
  std::string inputs_dir() const { return scratch_ + "/inputs"; }
  // Empties work/, tmp/ and the invocation input/output staging areas.
  void clear_scratch();
  // Resident memory of the worker process, when known.
  std::optional<uint64_t> rss_bytes() const;

 private:
  auto stop_log_reader() -> void;
  void read_logs(bool drain);
  void send(int fd, const std::string& frame);

  pid_t pid_ = -1;
  int data_fd_ = -1;
  int control_fd_ = -1;
  int out_fd_ = -1;
  int err_fd_ = -1;
  bool alive_ = false;
  std::string scratch_;
  // Darwin's permission repair runs with the same authority as the Card.
  std::function<void(int, const char*)> recover_permissions_;
  uint64_t call_id_ = 0;
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
    uint64_t call_id, const std::string& run_id, const CardPath& card,
    uint64_t invocation, std::span<const Value> inputs,
    const std::string& params_bfbs, const std::string& params_root,
    const std::string& params, const std::vector<types::PortType>& outputs,
    const std::string& work, const std::string& tmp, const std::string& out,
    const std::vector<std::pair<std::string, std::string>>& env,
    std::optional<uint64_t> deadline_ms);

}  // namespace tsugumi
