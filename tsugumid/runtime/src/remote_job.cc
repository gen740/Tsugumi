#include "remote_job.hh"

#include "tsugumi/protocol/trace_codec.hh"

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
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

#include "model.hh"
#include "tsugumi/rpc/unix_socket.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi {
namespace {

namespace R = Tsugumi::RemoteWorker;
struct Connection {
  int fd;
  ~Connection() {
    if (fd >= 0) ::close(fd);
  }
};

auto check_broker_stop(std::stop_token stopped, std::stop_token cancel,
                       bool upload) -> void {
  if (stopped.stop_requested())
    throw BrokerDisconnected("execution broker stopped for shutdown");
  if (cancel.stop_requested()) {
    if (upload)
      throw Failed(FailureKind::cancelled, "remote upload cancelled");
    throw BrokerDisconnected("execution broker request interrupted");
  }
}

struct BrokerIo {
  std::stop_token stopped, cancel;
  int wake;
  bool upload;

  auto check() const -> void { check_broker_stop(stopped, cancel, upload); }

  auto wait(int fd, short events) const -> void {
    pollfd ready[]{{fd, events, 0}, {wake, POLLIN, 0}};
    int n;
    do {
      n = ::poll(ready, 2, -1);
    } while (n < 0 && errno == EINTR);
    check();
    if (n < 0)
      throw BrokerDisconnected("execution broker socket poll failed");
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
    if (path.empty() || path.front() != '/')
      throw BrokerDisconnected("invalid execution broker Unix socket path");
    sockaddr_un address{};
    try {
      address = unix_socket::Address(std::string(path));
    } catch (const std::runtime_error& error) {
      throw BrokerDisconnected(error.what());
    }
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
  if (j->log_bytes.size() > workerd::kLogChunkBytes)
    throw BrokerDisconnected("remote log slice exceeds its limit");
  return {state,
          j->fingerprint,
          j->result_sha256,
          j->diagnostic,
          j->transfer_name,
          j->transfer_done,
          j->transfer_total,
          j->log_truncated,
          j->log_offset,
          j->log_size,
          std::string(j->log_bytes.begin(), j->log_bytes.end())};
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
                              std::stop_token cancel) const
    -> R::ReplyT {
  q.core_life_id = core_life_id_;
  const bool upload = q.body.type == R::RequestBody::PutBlobRequest;
  int wake[2];
  if (::pipe(wake) < 0)
    throw BrokerDisconnected("cannot open execution broker wake pipe");
  Connection reader{wake[0]}, writer{wake[1]};
  if (::fcntl(reader.fd, F_SETFD, FD_CLOEXEC) < 0 ||
      ::fcntl(writer.fd, F_SETFD, FD_CLOEXEC) < 0)
    throw BrokerDisconnected("cannot configure execution broker wake pipe");
  const BrokerIo io{stopped_.get_token(), cancel, reader.fd, upload};
  const auto wake_io = [&] {
    while (::write(writer.fd, "x", 1) < 0 && errno == EINTR) {}
  };
  std::stop_callback shutdown(io.stopped, wake_io);
  std::stop_callback interrupt(io.cancel, wake_io);
  auto endpoint = endpoint_;
  if (q.body.AsCancelRequest()) {
    if (endpoint.starts_with("unix:")) endpoint += ".c";
    if (endpoint == "vsock:1027") endpoint = "vsock:1028";
  }
  Connection socket{connect_broker(endpoint, !capability_file_.empty(), io)};
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
  trace::Link link;
  trace::Observe(link, "project core", "hostd execution broker", true, bytes,
                 &trace::kFlatBufferCodec);
  std::string prefix(4, '\0');
  read_exact(socket.fd, prefix.data(), prefix.size(), io);
  std::string response(workerd::frame_length(prefix), '\0');
  read_exact(socket.fd, response.data(), response.size(), io);
  auto incoming = workerd::decode(response);
  trace::Observe(link, "hostd execution broker", "project core", false, response,
                 &trace::kFlatBufferCodec);
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
auto RemoteJobClient::catalog(std::stop_token stop) const -> std::vector<RemoteTarget> {
  if (!enabled()) return {};
  auto reply = request(command(R::CatalogRequestT{}), stop);
  const auto* catalog = reply.body.AsCatalog();
  if (!catalog) throw BrokerDisconnected("expected execution broker catalog");
  std::vector<RemoteTarget> targets;
  for (const auto& t : catalog->services) {
    const auto& c = *t->capabilities;
    targets.push_back({t->id, c.native_system, c.systems, c.server_id,
                       t->available, c.resource_limits, c.max_jobs, t->grade,
                       t->unavailable_reason});
  }
  return targets;
}
auto RemoteJobClient::probe(std::string_view service,
                            std::string_view registered_server,
                            std::stop_token stop) const
    -> RemoteProbe {
  R::ReplyT reply;
  try {
    reply = request(command(R::ProbeRequestT{}, service), stop);
  } catch (const BrokerRequestRejected& error) {
    throw Failed(FailureKind::unsupported, error.what());
  } catch (const BrokerDisconnected&) {
    if (stop.stop_requested())
      throw Failed(FailureKind::cancelled, "worker selection cancelled");
    throw;
  }
  const auto& c = *reply.body.AsCapabilities();
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
                               std::stop_token cancel,
                               const TransferProgress& progress) const -> void {
  if (size > workerd::kMaxBlob)
    throw std::runtime_error("upload exceeds 64 GiB");
  uint64_t offset = 0;
  if (progress) progress(0, size);
  do {
    check_broker_stop(stopped_.get_token(), cancel, true);
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
                               std::stop_token stop,
                               const TransferProgress& progress) const -> void {
  crypto::Sha256 hash;
  uint64_t offset = 0, total = 0;
  do {
    check_broker_stop(stopped_.get_token(), stop, false);
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
                             std::string_view spec, std::stop_token stop) const -> RemoteJobState {
  R::SubmitRequestT body;
  body.spec.assign(spec.begin(), spec.end());
  return job_state(request(command(std::move(body), service, instance, job), stop));
}
auto RemoteJobClient::status(std::string_view service,
                             std::string_view instance,
                             std::string_view job, uint64_t log_offset,
                             std::stop_token stop) const -> RemoteJobState {
  R::StatusRequestT body;
  body.log_offset = log_offset;
  body.log_limit = workerd::kLogChunkBytes;
  return job_state(
      request(command(std::move(body), service, instance, job), stop));
}
auto RemoteJobClient::cancel(std::string_view service,
                             std::string_view instance, std::string_view job,
                             std::string_view fingerprint,
                             std::optional<uint64_t> log_offset) const
    -> RemoteJobState {
  R::CancelRequestT body;
  body.fingerprint = fingerprint;
  if (log_offset) {
    body.log_offset = *log_offset;
    body.log_limit = workerd::kLogChunkBytes;
  }
  return job_state(request(command(std::move(body), service, instance, job)));
}
auto RemoteJobClient::release(std::string_view service,
                              std::string_view instance, std::string_view job,
                              std::string_view fingerprint) const -> void {
  R::ReleaseRequestT body;
  body.fingerprint = fingerprint;
  require_ack(request(command(std::move(body), service, instance, job)));
}
auto RemoteJobClient::card_secrets(const std::vector<std::string>& names,
                                   std::stop_token stop) const
    -> std::vector<std::pair<std::string, std::string>> {
  if (!enabled())
    throw BrokerDisconnected("this project has no connection to hostd");
  R::CardSecretsRequestT body;
  body.names = names;
  auto reply = request(command(std::move(body)), stop);
  auto* secrets = reply.body.AsCardSecrets();
  if (!secrets) throw BrokerDisconnected("expected Card secrets");
  std::vector<std::pair<std::string, std::string>> out;
  for (auto& secret : secrets->secrets)
    if (std::ranges::find(names, secret->name) != names.end())
      out.emplace_back(std::move(secret->name), std::move(secret->value));
  return out;
}
}  // namespace tsugumi
