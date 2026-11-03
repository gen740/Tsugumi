// Core side of one tsugumi-worker process.
#pragma once

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "model.hh"
#include "nix.hh"

namespace tsugumi {

// Incremental UTF-8 decoder: emits complete characters only and replaces
// invalid sequences with U+FFFD, so chunk boundaries never split or lose text.
class Utf8Decoder {
 public:
  std::string feed(std::string_view bytes);
  std::string finish();

 private:
  std::string pending_;
};

struct WorkerSpawn {
  std::string worker_binary;
  std::string bash;
  CardBuild build;
  // The worker's own folder: work/ and tmp/ for Cards without filesystem:
  // true, scripts/ for preRun / a bash run / postRun. Created with the worker
  // and removed when it ends, so it lives as long as the worker.
  std::string scratch;
  bool sandbox = true;
  // Read-only binds, fixed for the worker's life: the project's run and saved
  // input trees (where every File / Directory input lives), so that a worker
  // serves any input. Output folders arrive as descriptors per call.
  std::vector<std::string> ro_paths;
  // Covered with an empty tmpfs for sandbox: false Cards (the control folder
  // with the RPC token, tsugumid's state); ro paths and the scratch folder
  // inside them are bound again on top.
  std::vector<std::string> hidden_paths;
  // Experimental, per Card: outside network (otherwise the worker has its
  // own network namespace with nothing in it) and folders of the Mac.
  bool network = false;
  struct Bind {
    std::string source, target;
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
 public:
  using LogSink = std::function<void(LogStream stream, const std::string& text)>;
  using DisplaySink = std::function<void(const DisplayData&)>;

  // Throws Failed(permission_denied) when the sandbox cannot be applied.
  explicit WorkerProc(const WorkerSpawn& spawn);
  ~WorkerProc();
  WorkerProc(const WorkerProc&) = delete;

  // Sends one call (with `fds` attached; the caller keeps and closes them) and
  // waits for its result. `cancel` is polled; on cancel or deadline the worker
  // gets WorkerCancel, then the process group is killed after one second.
  WorkerOutcome call(const std::string& frame, uint64_t call_id, const std::vector<int>& fds,
                     const std::atomic<bool>* cancel,
                     std::optional<std::chrono::steady_clock::time_point> deadline,
                     const DisplaySink& display);
  uint64_t next_call_id() { return ++call_id_; }
  // Where the worker's output goes while it serves the current call.
  void set_logs(LogSink logs);
  // The process still runs (checked without reaping it).
  bool alive() const;
  void terminate();
  // Under bubblewrap the Card sees only its binds: project folders of a call
  // are passed as descriptors (/proc/self/fd/<n>), not by path.
  bool passes_directories() const { return passes_dirs_; }
  std::string work_dir() const { return scratch_ + "/work"; }
  std::string tmp_dir() const { return scratch_ + "/tmp"; }
  // Empties work/ and tmp/ (only what a call left there).
  void clear_scratch();
  // Resident memory of the worker process, when known.
  std::optional<uint64_t> rss_bytes() const;
  pid_t pid() const { return pid_; }

 private:
  void read_logs(bool drain);
  void send(int fd, const std::string& frame, const std::vector<int>& fds = {});

  pid_t pid_ = -1;
  int data_fd_ = -1, control_fd_ = -1, out_fd_ = -1, err_fd_ = -1;
  bool alive_ = false;
  bool passes_dirs_ = false;
  std::string scratch_;
  uint64_t call_id_ = 0;
  LogSink logs_;
  std::mutex log_mu_;
  Utf8Decoder out_dec_, err_dec_;
  std::thread log_thread_;
  std::atomic<bool> stop_logs_{false};
};

// Builds a WorkerMessage{call_id, WorkerCall} frame.
std::string worker_call_frame(uint64_t call_id, const std::string& run_id, const CardPath& card,
                              uint64_t invocation, const std::vector<Value>& inputs,
                              const std::string& params_bfbs, const std::string& params_root,
                              const std::string& params, const std::vector<types::PortType>& outputs,
                              const std::string& work, const std::string& tmp, const std::string& out,
                              const std::vector<std::pair<std::string, std::string>>& env,
                              std::optional<uint64_t> deadline_ms, const std::vector<int32_t>& passed_fds = {});

}  // namespace tsugumi
