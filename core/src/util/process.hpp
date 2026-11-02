// Child process management with process groups, explicit environments and
// cooperative cancellation.
#pragma once

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tsugumi::proc {

struct Spec {
  std::vector<std::string> argv;
  std::vector<std::string> env;  // "K=V"; the parent environment is never inherited
  std::string cwd;
  // Extra descriptors mapped into the child: {child_fd, parent_fd}.
  std::vector<std::pair<int, int>> fds;
  int stdout_fd = -1;  // parent fd to dup onto 1 (-1: /dev/null)
  int stderr_fd = -1;  // parent fd to dup onto 2 (-1: /dev/null)
  std::optional<uid_t> uid;
  std::optional<gid_t> gid;
};

// Starts the child in its own process group. Throws on failure.
pid_t spawn(const Spec& spec);
// Sends `sig` to the whole process group of `pid`.
void kill_group(pid_t pid, int sig);
// Non-blocking wait; returns exit status when finished.
std::optional<int> try_wait(pid_t pid);
int wait(pid_t pid);

struct Result {
  int status = -1;  // raw wait status
  bool cancelled = false;
  bool timed_out = false;
  std::string out;  // stdout (captured)
};

// Runs a command to completion. stderr lines are forwarded to `on_stderr`.
// Cancellation and deadline terminate the whole process group (TERM, then
// KILL after `grace`).
Result run(const Spec& spec, const std::atomic<bool>* cancel,
           std::optional<std::chrono::steady_clock::time_point> deadline,
           const std::function<void(const std::string&)>& on_stderr,
           std::chrono::milliseconds grace = std::chrono::milliseconds(1000));

bool exited_ok(int status);
std::string describe(int status);

}  // namespace tsugumi::proc
