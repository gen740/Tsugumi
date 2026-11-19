#include "remote_job.hh"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/vm_sockets.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

#include "model.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi {
namespace {

namespace R = Tsugumi::RemoteWorker;
using namespace std::chrono_literals;
constexpr auto kStopCheckInterval = 100ms;

struct BrokerIo {
  const std::atomic<bool>& stopped;
  const std::atomic<bool>* cancel;
  bool upload;

  auto check() const -> void {
    if (stopped.load())
      throw BrokerDisconnected("execution broker stopped for shutdown");
    if (cancel && cancel->load()) {
      if (upload)
        throw Failed(FailureKind::cancelled, "remote upload cancelled");
      throw BrokerDisconnected("remote download paused for shutdown");
    }
  }

  auto wait(int fd, short events) const -> void {
    for (;;) {
      check();
      pollfd ready{fd, events, 0};
      const int n =
          ::poll(&ready, 1, static_cast<int>(kStopCheckInterval.count()));
      if (n > 0) return;
      if (n < 0 && errno != EINTR)
        throw BrokerDisconnected("execution broker socket poll failed");
    }
  }
};

struct Connection {
  int fd;
  ~Connection() {
    if (fd >= 0) ::close(fd);
  }
};

auto socket_for(int family, const sockaddr* address, socklen_t length,
                const BrokerIo& io) -> int {
  io.check();
  const int fd = ::socket(family, SOCK_STREAM, 0);
  if (fd < 0) throw BrokerDisconnected("cannot open execution broker socket");
  Connection socket{fd};
  if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
      ::fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    throw BrokerDisconnected("cannot configure execution broker socket");
#ifdef SO_NOSIGPIPE
  const int no_sigpipe = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe);
#endif
  bool connected = ::connect(fd, address, length) == 0;
  if (!connected && errno == EINPROGRESS) {
    io.wait(fd, POLLOUT);
    int error = 0;
    socklen_t size = sizeof error;
    connected = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
                error == 0;
  }
  if (!connected)
    throw BrokerDisconnected("cannot connect to execution broker");
  socket.fd = -1;
  return fd;
}

auto send_exact(int fd, std::string_view bytes, const BrokerIo& io) -> void {
#ifdef MSG_NOSIGNAL
  constexpr int flags = MSG_NOSIGNAL;
#else
  constexpr int flags = 0;
#endif
  while (!bytes.empty()) {
    io.check();
    const ssize_t n = ::send(fd, bytes.data(), bytes.size(), flags);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      io.wait(fd, POLLOUT);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0)
      throw BrokerDisconnected("execution broker disconnected during request");
    bytes.remove_prefix(static_cast<size_t>(n));
  }
}

auto read_exact(int fd, char* out, size_t size, const BrokerIo& io) -> void {
  while (size > 0) {
    io.check();
    const ssize_t n = ::read(fd, out, size);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      io.wait(fd, POLLIN);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0)
      throw BrokerDisconnected("execution broker disconnected during response");
    out += n;
    size -= static_cast<size_t>(n);
  }
}

auto environment(const char* name) -> std::string {
  const char* value = std::getenv(name);
  return value ? value : "";
}

auto connect_broker(std::string_view endpoint,
                    [[maybe_unused]] bool has_capability, const BrokerIo& io)
    -> int {
  if (endpoint.starts_with("unix:")) {
    const std::string_view path = endpoint.substr(5);
    sockaddr_un address{};
    if (path.empty() || path.front() != '/' ||
        path.size() >= sizeof address.sun_path)
      throw BrokerDisconnected("invalid execution broker Unix socket path");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    return socket_for(AF_UNIX, reinterpret_cast<const sockaddr*>(&address),
                      sizeof address, io);
  }
#if defined(__linux__)
  if (endpoint.starts_with("vsock:")) {
    const std::string_view text = endpoint.substr(6);
    uint32_t port = 0;
    auto [end, ec] =
        std::from_chars(text.data(), text.data() + text.size(), port);
    if (ec != std::errc{} || end != text.data() + text.size() || port == 0 ||
        !has_capability)
      throw BrokerDisconnected(
          "invalid authenticated execution broker vsock endpoint");
    sockaddr_vm address{};
    address.svm_family = AF_VSOCK;
    address.svm_cid = VMADDR_CID_HOST;
    address.svm_port = port;
    return socket_for(AF_VSOCK, reinterpret_cast<const sockaddr*>(&address),
                      sizeof address, io);
  }
#endif
  throw BrokerDisconnected("no execution broker endpoint is configured");
}

template <typename T>
auto command(T body, std::string_view service = {},
             std::string_view instance = {}, std::string_view job = {})
    -> R::RequestT {
  R::RequestT q;
  q.service = service;
  q.instance_id = instance;
  q.job_id = job;
  q.body.Set(std::move(body));
  return q;
}
auto job_state(const R::ReplyT& reply) -> RemoteJobState {
  const auto* j = reply.body.AsJobStatus();
  if (!j) throw BrokerDisconnected("expected remote job status");
  const auto* name = R::EnumNameJobState(j->state);
  if (!name || !*name) throw BrokerDisconnected("unknown remote job state");
  std::string state(name);
  std::ranges::transform(state, state.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return {state,
          j->fingerprint,
          j->result_sha256,
          j->diagnostic,
          j->transfer_name,
          j->transfer_done,
          j->transfer_total,
          j->log_truncated};
}
auto require_ack(const R::ReplyT& reply) -> void {
  if (!reply.body.AsAck())
    throw BrokerDisconnected("expected remote worker acknowledgement");
}
}  // namespace

RemoteJobClient::RemoteJobClient()
    : core_life_id_(crypto::random_token(32)),
      endpoint_(environment("TSUGUMI_EXECUTION_BROKER")),
      capability_file_(
          environment("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE")) {}

RemoteJobClient::~RemoteJobClient() { stop(); }

auto RemoteJobClient::request(R::RequestT q,
                              const std::atomic<bool>* cancel) const
    -> R::ReplyT {
  q.core_life_id = core_life_id_;
  const bool upload = q.body.type == R::RequestBody::PutBlobRequest;
  const BrokerIo io{stopped_, cancel, upload};
  Connection socket{connect_broker(endpoint_, !capability_file_.empty(), io)};
  auto capability = fs::read_file(capability_file_);
  if (!capability || capability->empty() || capability->size() > 256)
    throw BrokerDisconnected("cannot read execution broker capability");
  R::BrokerRequestT broker;
  broker.capability = *capability;
  broker.request = std::make_unique<R::RequestT>(std::move(q));
  R::MessageT message;
  message.payload.Set(std::move(broker));
  const auto bytes = workerd::encode(std::move(message));
  send_exact(socket.fd,
             workerd::length_prefix(static_cast<uint32_t>(bytes.size())), io);
  send_exact(socket.fd, bytes, io);
  ::shutdown(socket.fd, SHUT_WR);
  std::string prefix(4, '\0');
  read_exact(socket.fd, prefix.data(), prefix.size(), io);
  std::string response(workerd::frame_length(prefix), '\0');
  read_exact(socket.fd, response.data(), response.size(), io);
  auto incoming = workerd::decode(response);
  auto* reply = incoming->payload.AsReply();
  if (!reply) throw BrokerDisconnected("expected execution broker reply");
  if (const auto* error = reply->body.AsFailure()) {
    switch (error->code) {
      case R::ErrorCode::InstanceChanged:
        throw BrokerInstanceChanged(error->message);
      case R::ErrorCode::RegistrationChanged:
        throw BrokerDisconnected(error->message);
      case R::ErrorCode::Rejected:
      case R::ErrorCode::Invalid:
        throw BrokerRequestRejected(error->message);
      default:
        throw std::runtime_error("remote worker: " + error->message);
    }
  }
  return std::move(*reply);
}
auto RemoteJobClient::catalog() const -> std::vector<RemoteTarget> {
  if (!enabled()) return {};
  auto reply = request(command(R::CatalogRequestT{}));
  const auto* catalog = reply.body.AsCatalog();
  if (!catalog) throw BrokerDisconnected("expected execution broker catalog");
  std::vector<RemoteTarget> targets;
  for (const auto& t : catalog->services) {
    if (!t || !t->capabilities)
      throw BrokerDisconnected("execution broker service missing capabilities");
    const auto& c = *t->capabilities;
    targets.push_back({t->id, c.native_system, c.systems, c.server_id,
                       t->available, c.resource_limits, c.max_jobs, t->grade,
                       t->unavailable_reason});
  }
  return targets;
}
auto RemoteJobClient::probe(std::string_view service,
                            std::string_view registered_server) const
    -> RemoteProbe {
  R::ReplyT reply;
  try {
    reply = request(command(R::ProbeRequestT{}, service));
  } catch (const BrokerRequestRejected& error) {
    throw Failed(FailureKind::unsupported, error.what());
  }
  const auto* caps = reply.body.AsCapabilities();
  if (!caps)
    throw BrokerDisconnected("expected execution broker capabilities");
  const auto& c = *caps;
  // The catalog may have been replaced before the broker received this probe.
  if (c.server_id != registered_server)
    throw Failed(FailureKind::unsupported,
                 "The remote worker's server ID changed. Verify the registered "
                 "endpoint.");
  return {c.native_system,   c.instance_id, c.systems,
          c.resource_limits, c.max_jobs,    c.active_jobs};
}
auto RemoteJobClient::put_blob(std::string_view service,
                               std::string_view instance, std::string_view job,
                               std::string_view digest, uint64_t size,
                               std::istream& contents,
                               const std::atomic<bool>* cancel,
                               const TransferProgress& progress) const -> void {
  if (size > workerd::kMaxBlob)
    throw std::runtime_error("upload exceeds 64 GiB");
  uint64_t offset = 0;
  if (progress) progress(0, size);
  do {
    BrokerIo{stopped_, cancel, true}.check();
    R::PutBlobRequestT chunk;
    chunk.digest = digest;
    chunk.total = size;
    chunk.offset = offset;
    chunk.bytes.resize(static_cast<size_t>(
        std::min<uint64_t>(size - offset, workerd::kChunkBytes)));
    if (!chunk.bytes.empty()) {
      contents.read(reinterpret_cast<char*>(chunk.bytes.data()),
                    static_cast<std::streamsize>(chunk.bytes.size()));
      if (static_cast<size_t>(contents.gcount()) != chunk.bytes.size())
        throw std::runtime_error("truncated upload");
    }
    offset += chunk.bytes.size();
    require_ack(
        request(command(std::move(chunk), service, instance, job), cancel));
    if (progress) progress(offset, size);
  } while (offset < size);
}
auto RemoteJobClient::get_blob(std::string_view service,
                               std::string_view instance, std::string_view job,
                               std::string_view digest,
                               std::ostream& destination, uint64_t limit,
                               std::optional<uint64_t> expected_size,
                               const std::atomic<bool>* stop,
                               const TransferProgress& progress) const -> void {
  crypto::Sha256 hash;
  uint64_t offset = 0, total = 0;
  do {
    BrokerIo{stopped_, stop, false}.check();
    R::GetBlobRequestT chunk;
    chunk.digest = digest;
    chunk.offset = offset;
    chunk.limit = workerd::kChunkBytes;
    auto reply =
        request(command(std::move(chunk), service, instance, job), stop);
    const auto* body = reply.body.AsBlobChunk();
    if (!body || body->digest != digest || body->offset != offset ||
        (offset ? body->total != total
                : !expected_size && body->total > limit) ||
        body->bytes.size() > body->total - offset ||
        body->bytes.size() > workerd::kChunkBytes ||
        (body->bytes.empty() && offset < body->total))
      throw BrokerDisconnected("invalid remote blob chunk");
    if (!offset && expected_size && body->total != *expected_size)
      throw std::runtime_error("remote result size mismatch");
    total = body->total;
    if (progress && !offset) progress(0, total);
    hash.update(body->bytes.data(), body->bytes.size());
    destination.write(reinterpret_cast<const char*>(body->bytes.data()),
                      static_cast<std::streamsize>(body->bytes.size()));
    if (!destination) throw std::runtime_error("cannot store remote blob");
    offset += body->bytes.size();
    if (progress) progress(offset, total);
  } while (offset < total);
  if (hash.finish_hex() != digest)
    throw std::runtime_error("remote blob failed SHA-256 verification");
}
auto RemoteJobClient::submit(std::string_view service,
                             std::string_view instance, std::string_view job,
                             std::string_view spec) const -> RemoteJobState {
  R::SubmitRequestT body;
  body.spec.assign(spec.begin(), spec.end());
  return job_state(request(command(std::move(body), service, instance, job)));
}
auto RemoteJobClient::status(std::string_view service,
                             std::string_view instance,
                             std::string_view job) const -> RemoteJobState {
  return job_state(
      request(command(R::StatusRequestT{}, service, instance, job)));
}
auto RemoteJobClient::cancel(std::string_view service,
                             std::string_view instance, std::string_view job,
                             std::string_view fingerprint) const
    -> RemoteJobState {
  R::CancelRequestT body;
  body.fingerprint = fingerprint;
  return job_state(request(command(std::move(body), service, instance, job)));
}
auto RemoteJobClient::release(std::string_view service,
                              std::string_view instance, std::string_view job,
                              std::string_view fingerprint) const -> void {
  R::ReleaseRequestT body;
  body.fingerprint = fingerprint;
  require_ack(request(command(std::move(body), service, instance, job)));
}
}  // namespace tsugumi
