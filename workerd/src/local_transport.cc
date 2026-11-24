#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "tsugumi/protocol/trace_codec.hh"
#include "tsugumi/workerd/local.hh"
#include "tsugumi/workerd/protocol.hh"

namespace tsugumi::workerd {
namespace {
constexpr uint32_t kMaxLocalFrame = 1 << 20;
auto transfer(int fd, void* buffer, size_t length, bool writing) -> void {
  auto* bytes = static_cast<char*>(buffer);
  while (length) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto count = writing ? ::send(fd, bytes, length, flags)
                               : ::recv(fd, bytes, length, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("local workerd connection closed");
    bytes += count;
    length -= static_cast<size_t>(count);
  }
}
}  // namespace

auto clear_spawn_secrets(Tsugumi::LocalWorker::MessageT& message) -> void {
  if (!message.spawn) return;
  for (auto& entry : message.spawn->secrets) clear_secret(entry->value);
  message.spawn->secrets.clear();
}

LocalPacket::~LocalPacket() {
  if (message) clear_spawn_secrets(*message);
  for (int fd : descriptors)
    if (fd >= 0) ::close(fd);
}

auto send_local(int fd, const Tsugumi::LocalWorker::MessageT& message,
                const std::array<int, 4>* descriptors, trace::Link* link) -> void {
  flatbuffers::FlatBufferBuilder builder;
  Tsugumi::LocalWorker::FinishMessageBuffer(
      builder, Tsugumi::LocalWorker::Message::Pack(builder, &message));
  const auto size = builder.GetSize();
  struct Wipe {
    std::span<char> bytes;
    ~Wipe() { wipe_secret(bytes); }
  } wipe{{reinterpret_cast<char*>(builder.GetBufferPointer()), size}};
  if (!size || size > kMaxLocalFrame)
    throw std::runtime_error("local workerd message too large");
  const std::array<unsigned char, 4> prefix{
      static_cast<unsigned char>(size), static_cast<unsigned char>(size >> 8),
      static_cast<unsigned char>(size >> 16),
      static_cast<unsigned char>(size >> 24)};
  iovec data{const_cast<unsigned char*>(prefix.data()), prefix.size()};
  alignas(cmsghdr) char control[CMSG_SPACE(4 * sizeof(int))]{};
  msghdr header{};
  header.msg_iov = &data;
  header.msg_iovlen = 1;
  if (descriptors) {
    header.msg_control = control;
    header.msg_controllen = sizeof control;
    auto* rights = CMSG_FIRSTHDR(&header);
    rights->cmsg_level = SOL_SOCKET;
    rights->cmsg_type = SCM_RIGHTS;
    rights->cmsg_len = CMSG_LEN(4 * sizeof(int));
    std::memcpy(CMSG_DATA(rights), descriptors->data(), 4 * sizeof(int));
  }
#ifdef MSG_NOSIGNAL
  constexpr int flags = MSG_NOSIGNAL;
#else
  constexpr int flags = 0;
#endif
  ssize_t sent;
  do {
    sent = ::sendmsg(fd, &header, flags);
  } while (sent < 0 && errno == EINTR);
  if (sent <= 0)
    throw std::runtime_error("cannot hand worker connection to core");
  transfer(fd, const_cast<unsigned char*>(prefix.data()) + sent,
           prefix.size() - sent, true);
  transfer(fd, builder.GetBufferPointer(), size, true);
  trace::Observe(link, "project core", "Card host", true,
                 {reinterpret_cast<const char*>(builder.GetBufferPointer()), size},
                 &trace::kFlatBufferCodec);
}

auto receive_local(int fd, LocalPacket& packet, trace::Link* link) -> bool {
  std::array<unsigned char, 4> prefix{};
  iovec data{prefix.data(), prefix.size()};
  alignas(cmsghdr) char control[CMSG_SPACE(4 * sizeof(int))]{};
  msghdr header{};
  header.msg_iov = &data;
  header.msg_iovlen = 1;
  header.msg_control = control;
  header.msg_controllen = sizeof control;
  ssize_t received;
  do {
    received = ::recvmsg(fd, &header, 0);
  } while (received < 0 && errno == EINTR);
  if (received == 0) return false;
  if (received < 0)
    throw std::runtime_error("cannot receive local workerd message");
  size_t count = 0;
  for (auto* rights = CMSG_FIRSTHDR(&header); rights;
       rights = CMSG_NXTHDR(&header, rights)) {
    if (rights->cmsg_level != SOL_SOCKET || rights->cmsg_type != SCM_RIGHTS)
      continue;
    const auto n = (rights->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    const auto* fds = reinterpret_cast<const int*>(CMSG_DATA(rights));
    for (size_t i = 0; i < n; ++i) {
      if (count < packet.descriptors.size()) {
        packet.descriptors[count++] = fds[i];
        ::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
      } else
        ::close(fds[i]);
    }
  }
  if ((header.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) || (count && count != 4))
    throw std::runtime_error("invalid worker descriptor handover");
  transfer(fd, prefix.data() + received, prefix.size() - received, false);
  const uint32_t size = uint32_t(prefix[0]) | (uint32_t(prefix[1]) << 8) |
                        (uint32_t(prefix[2]) << 16) |
                        (uint32_t(prefix[3]) << 24);
  if (!size || size > kMaxLocalFrame)
    throw std::runtime_error("invalid local workerd frame length");
  std::string bytes(size, '\0');
  struct Wipe {
    std::string& bytes;
    ~Wipe() { clear_secret(bytes); }
  } wipe{bytes};
  transfer(fd, bytes.data(), bytes.size(), false);
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(bytes.data()),
                                 bytes.size());
  if (!Tsugumi::LocalWorker::VerifyMessageBuffer(verifier))
    throw std::runtime_error("invalid local workerd message");
  trace::Observe(link, "Card host", "project core", false, bytes,
                 &trace::kFlatBufferCodec);
  packet.message.reset(
      Tsugumi::LocalWorker::GetMessage(bytes.data())->UnPack());
  return true;
}
}  // namespace tsugumi::workerd
