// Child process management with process groups, explicit environments and
// cooperative cancellation.
#pragma once

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tsugumi::proc {

struct Spec {
  std::vector<std::string> argv;
  std::vector<std::string>
      env;  // "K=V"; the parent environment is never inherited
  std::string cwd;
  std::string cgroup_procs;  // Linux: join before exec if supplied
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

struct LineChunk {
  std::span<const std::string> lines;
  bool truncated = false;
};

struct Result {
  int status = -1;  // raw wait status
  bool cancelled = false;
  bool timed_out = false;
  std::string out;  // stdout (captured)
};

enum class LogFraming { stderr_lines, stdout_size_prefixed };

// Runs a command to completion. Complete stderr lines are delivered in order,
// collected across pipe reads: a delivery waits at most a short interval after
// its first line, so the number of deliveries does not follow how the OS
// splits the child's output into reads. At EOF, any final unterminated line is
// delivered too. With a line
// limit, an oversized line ends delivery after sending the accepted prefix
// with truncated=true. The pipe is still drained and the child reaped.
// Cancellation and deadline terminate the whole process group (TERM, then
// KILL after `grace`). With stdout_size_prefixed, stdout carries records
// prefixed by a little-endian uint32 size and stderr is captured as
// diagnostics.
Result run(const Spec& spec, const std::atomic<bool>* cancel,
           std::optional<std::chrono::steady_clock::time_point> deadline,
           const std::function<void(LineChunk)>& on_stderr,
           std::optional<size_t> stderr_line_limit = std::nullopt,
           std::chrono::milliseconds grace = std::chrono::milliseconds(1000),
           LogFraming framing = LogFraming::stderr_lines);

bool exited_ok(int status);
std::string describe(int status);

// CPU time (user and system, ms) of the processes below `root`: the live ones
// and the children each of them has reaped. `root` itself is not counted.
// Linux reads /proc, macOS libproc. A process that left the tree by being
// reparented (an orphan) is no longer counted.
uint64_t descendants_cpu_ms(pid_t root);
// `root`'s own CPU time and that of the children it reaped, plus
// descendants_cpu_ms(root); nullopt when `root` cannot be read.
std::optional<uint64_t> tree_cpu_ms(pid_t root);

// The CPU time of the nix-daemon processes serving `client`'s store
// connections, with the builders below them and those they reaped (tree_cpu_ms
// of each). nix-daemon names such a process by its client's pid as the daemon
// sees it: in argv[1] (Nix 2.34) or as `nix-daemon.worker-<pid>` (Nix 2.36).
// Found only in a /proc that shows the daemon's processes, with `client` in
// that /proc's PID namespace; elsewhere 0. Linux only.
uint64_t nix_daemon_cpu_ms(pid_t client);

}  // namespace tsugumi::proc
