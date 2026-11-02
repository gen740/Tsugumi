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

#include "model.hpp"
#include "nix.hpp"

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
  std::string script_dir;  // where pre/script/post files are written
  bool sandbox = true;
  std::vector<std::string> ro_paths;  // extra read-only binds (input files)
  std::vector<std::string> rw_paths;  // work/tmp/out roots
  std::optional<uid_t> uid;
  std::optional<gid_t> gid;
};

struct WorkerOutcome {
  std::vector<std::pair<std::string, std::string>> outputs;
  std::optional<FailureData> failure;
};

class WorkerProc {
 public:
  using LogSink = std::function<void(LogStream stream, const std::string& text)>;
  using DisplaySink = std::function<void(const DisplayData&)>;

  // Throws Failed(permission_denied) when the sandbox cannot be applied.
  WorkerProc(const WorkerSpawn& spawn, LogSink logs);
  ~WorkerProc();
  WorkerProc(const WorkerProc&) = delete;

  // Sends one call and waits for its result. `cancel` is polled; on cancel or
  // deadline the worker gets WorkerCancel, then the process group is killed
  // after one second.
  WorkerOutcome call(const std::string& call_bytes_without_id, uint64_t call_id,
                     const std::atomic<bool>* cancel,
                     std::optional<std::chrono::steady_clock::time_point> deadline,
                     const DisplaySink& display);
  bool alive() const { return alive_; }
  void terminate();

 private:
  void read_logs(bool drain);
  void send(int fd, const std::string& frame);

  pid_t pid_ = -1;
  int data_fd_ = -1, control_fd_ = -1, out_fd_ = -1, err_fd_ = -1;
  bool alive_ = false;
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
                              std::optional<uint64_t> deadline_ms);

}  // namespace tsugumi
