// tsugumi-vmd's answers to project processes on /run/tsugumi-build-cpu
// (protocol/vm_project.fbs): the CPU time of the nix-daemon processes serving
// the connecting process. A project's core runs in a PID namespace of its own
// and cannot see the daemon's processes; nix-daemon names them by its
// client's pid as the guest sees it. The kernel gives the connecting
// process's pid in the guest's namespace to vmd too (SO_PEERCRED), so a
// request names no process and the two sides never translate pids.
#pragma once

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/ucred.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tsugumi/protocol/vm_project_generated.h"

namespace tsugumi::vm_guest {
namespace project_wire = Tsugumi::VMProject;

// A request is an empty table; a larger frame is not one.
inline constexpr uint32_t kMaxBuildCpuFrame = 256;
// Connections one uid may hold open, and all of them together: a project
// cannot keep the others from being answered.
inline constexpr size_t kBuildCpuPeersPerUid = 64;
inline constexpr size_t kBuildCpuPeers = 512;

struct BuildCpuPeer {
  pid_t pid = 0;  // in vmd's PID namespace
  uid_t uid = 0;
};

// The credentials of the process that connected `fd`, fixed at connect().
inline auto PeerOf(int fd) -> std::optional<BuildCpuPeer> {
#ifdef __linux__
  ucred cred{};
  socklen_t size = sizeof cred;
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) != 0)
    return std::nullopt;
  return BuildCpuPeer{cred.pid, cred.uid};
#elif defined(__APPLE__)
  xucred cred{};
  socklen_t size = sizeof cred;
  pid_t pid = 0;
  socklen_t pid_size = sizeof pid;
  if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &size) != 0 ||
      ::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &pid_size) != 0)
    return std::nullopt;
  return BuildCpuPeer{pid, cred.cr_uid};
#else
  (void)fd;
  return std::nullopt;
#endif
}

inline auto EncodeBuildCpuReply(const project_wire::BuildCpuReplyT& reply)
    -> std::string {
  flatbuffers::FlatBufferBuilder builder;
  builder.FinishSizePrefixed(
      project_wire::BuildCpuReply::Pack(builder, &reply));
  return {reinterpret_cast<const char*>(builder.GetBufferPointer()),
          builder.GetSize()};
}

// Answers every connection of `listener` on the calling thread, until the
// listener fails. Connections are non-blocking: a peer that sends a partial
// frame or does not read its replies holds up no other.
class BuildCpuServer {
 public:
  using Answer =
      std::function<project_wire::BuildCpuReplyT(const BuildCpuPeer&)>;

  BuildCpuServer(int listener, Answer answer)
      : listener_(listener), answer_(std::move(answer)) {}
  ~BuildCpuServer() {
    for (const auto& peer : peers_) ::close(peer.fd);
  }
  BuildCpuServer(const BuildCpuServer&) = delete;
  auto operator=(const BuildCpuServer&) -> BuildCpuServer& = delete;

  // Returns when the listener fails (or `stop` becomes readable).
  auto Run(int stop = -1) -> void {
    for (;;) {
      std::vector<pollfd> fds{{listener_, POLLIN, 0}, {stop, POLLIN, 0}};
      for (const auto& peer : peers_) fds.push_back({peer.fd, POLLIN, 0});
      if (::poll(fds.data(), fds.size(), -1) < 0) {
        if (errno == EINTR) continue;
        return;
      }
      if (fds[1].revents) return;
      for (size_t i = peers_.size(); i-- > 0;)
        if (fds[i + 2].revents && !Serve(peers_[i])) {
          ::close(peers_[i].fd);
          peers_.erase(peers_.begin() + static_cast<ptrdiff_t>(i));
        }
      if (fds[0].revents & (POLLERR | POLLNVAL)) return;
      if (fds[0].revents & POLLIN) Accept();
    }
  }

 private:
  struct Connection {
    int fd = -1;
    BuildCpuPeer peer;
    std::string pending;
  };

  auto Accept() -> void {
#ifdef __linux__
    const int fd = ::accept4(listener_, nullptr, nullptr,
                             SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
    const int fd = ::accept(listener_, nullptr, nullptr);
    if (fd >= 0) {
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
      ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
      const int on = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    }
#endif
    if (fd < 0) return;
    const auto peer = PeerOf(fd);
    size_t same = 0;
    for (const auto& other : peers_)
      if (peer && other.peer.uid == peer->uid) ++same;
    if (!peer || peers_.size() >= kBuildCpuPeers ||
        same >= kBuildCpuPeersPerUid) {
      ::close(fd);
      return;
    }
    peers_.push_back({fd, *peer, {}});
  }

  // False when the connection ends.
  auto Serve(Connection& connection) -> bool {
    char buffer[512];
    const auto count = ::read(connection.fd, buffer, sizeof buffer);
    if (count < 0) return errno == EINTR || errno == EAGAIN;
    if (count == 0) return false;
    connection.pending.append(buffer, static_cast<size_t>(count));
    while (connection.pending.size() >= sizeof(uint32_t)) {
      const auto size = flatbuffers::ReadScalar<uint32_t>(
          connection.pending.data());
      if (size == 0 || size > kMaxBuildCpuFrame) return false;
      if (connection.pending.size() < sizeof(uint32_t) + size) break;
      // Verified with its prefix: the builder aligns the whole frame.
      flatbuffers::Verifier verifier(
          reinterpret_cast<const uint8_t*>(connection.pending.data()),
          sizeof(uint32_t) + size);
      if (!verifier.VerifySizePrefixedBuffer<project_wire::BuildCpuRequest>(
              nullptr))
        return false;
      connection.pending.erase(0, sizeof(uint32_t) + size);
      const auto reply = EncodeBuildCpuReply(answer_(connection.peer));
      // A reply is far smaller than a socket buffer: one that does not fit
      // means the peer stopped reading.
      const auto sent = ::send(connection.fd, reply.data(), reply.size(),
                               MSG_DONTWAIT | kNoSignal);
      if (sent != static_cast<ssize_t>(reply.size())) return false;
    }
    return true;
  }

#ifdef MSG_NOSIGNAL
  static constexpr int kNoSignal = MSG_NOSIGNAL;
#else
  static constexpr int kNoSignal = 0;
#endif

  int listener_;
  Answer answer_;
  std::vector<Connection> peers_;
};

}  // namespace tsugumi::vm_guest
