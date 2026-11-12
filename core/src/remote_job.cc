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
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

#include "schema.hh"
#include "model.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {

constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kWorkerContract = 2;
constexpr uint32_t kMaxHeaderBytes = 64 * 1024;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto kConnectTimeout = 5s;
constexpr auto kControlTimeout = 30s;
constexpr auto kIdleTimeout = 30s;
constexpr auto kTransferTimeout = 24h;
constexpr auto kStopCheckInterval = 100ms;

struct BrokerIo {
  const std::atomic<bool>& stopped;
  const std::atomic<bool>* cancel;
  bool upload;

  auto check(Clock::time_point deadline) const -> void {
    if (stopped.load()) throw BrokerDisconnected("execution broker stopped for shutdown");
    if (cancel && cancel->load()) {
      if (upload) throw Failed(FailureKind::cancelled, "remote upload cancelled");
      throw BrokerDisconnected("remote download paused for shutdown");
    }
    if (Clock::now() >= deadline) throw BrokerDisconnected("execution broker deadline exceeded");
  }

  auto wait(int fd, short events, Clock::time_point deadline) const -> void {
    for (;;) {
      check(deadline);
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
      const auto wait_for = std::clamp(remaining, 0ms, kStopCheckInterval);
      pollfd ready{fd, events, 0};
      const int n = ::poll(&ready, 1, static_cast<int>(wait_for.count()));
      if (n > 0) return;
      if (n < 0 && errno != EINTR) throw BrokerDisconnected("execution broker socket poll failed");
    }
  }
};

struct Connection {
  int fd;
  ~Connection() { if (fd >= 0) ::close(fd); }
};

auto socket_for(int family, const sockaddr* address, socklen_t length, const BrokerIo& io,
                Clock::time_point deadline) -> int {
  io.check(deadline);
  const int fd = ::socket(family, SOCK_STREAM, 0);
  if (fd < 0) throw BrokerDisconnected("cannot open execution broker socket");
  Connection socket{fd};
  if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    throw BrokerDisconnected("cannot configure execution broker socket");
#ifdef SO_NOSIGPIPE
  const int no_sigpipe = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe);
#endif
  bool connected = ::connect(fd, address, length) == 0;
  if (!connected && errno == EINPROGRESS) {
    io.wait(fd, POLLOUT, deadline);
    int error = 0;
    socklen_t size = sizeof error;
    connected = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
  }
  if (!connected) throw BrokerDisconnected("cannot connect to execution broker");
  socket.fd = -1;
  return fd;
}

auto send_exact(int fd, std::string_view bytes, const BrokerIo& io, Clock::time_point deadline) -> void {
#ifdef MSG_NOSIGNAL
  constexpr int flags = MSG_NOSIGNAL;
#else
  constexpr int flags = 0;
#endif
  auto idle = Clock::now() + kIdleTimeout;
  while (!bytes.empty()) {
    const auto limit = std::min(deadline, idle);
    io.check(limit);
    const ssize_t n = ::send(fd, bytes.data(), bytes.size(), flags);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      io.wait(fd, POLLOUT, limit);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw BrokerDisconnected("execution broker disconnected during request");
    bytes.remove_prefix(static_cast<size_t>(n));
    idle = Clock::now() + kIdleTimeout;
  }
}

auto read_exact(int fd, char* out, size_t size, const BrokerIo& io, Clock::time_point deadline) -> void {
  auto idle = Clock::now() + kIdleTimeout;
  while (size > 0) {
    const auto limit = std::min(deadline, idle);
    io.check(limit);
    const ssize_t n = ::read(fd, out, size);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      io.wait(fd, POLLIN, limit);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw BrokerDisconnected("execution broker disconnected during response");
    out += n;
    size -= static_cast<size_t>(n);
    idle = Clock::now() + kIdleTimeout;
  }
}

auto environment(const char* name) -> std::string {
  const char* value = std::getenv(name);
  return value ? value : "";
}

template <typename Field>
auto string_or_empty(const Field& field) -> std::string {
  if (const auto* value = schema::get(field)) return *value;
  return {};
}

struct TargetJson {
  schema::Opt<"id", schema::v::String> id;
  schema::Opt<"server_id", schema::v::String> server_id;
  schema::Opt<"native_system", schema::v::String> native_system;
  schema::Opt<"systems", schema::v::Array<schema::v::String>> systems;
  schema::Opt<"executor", schema::v::String> executor;
  schema::Opt<"isolation", schema::v::String> isolation;
  schema::Opt<"resource_limits", schema::v::Bool> resource_limits;
  schema::Opt<"max_jobs", schema::v::Uint64> max_jobs;
  schema::Opt<"worker_contract", schema::v::Uint64> worker_contract;
  schema::Opt<"available", schema::v::Bool> available;
};

template <typename T>
auto daemon_capability_error(const T& target) -> std::string {
  const auto* contract = schema::get(target.worker_contract);
  if (!contract || *contract != kWorkerContract)
    return "Execution daemon worker contract is " + (contract ? std::to_string(*contract) : "unreported") +
           "; expected " + std::to_string(kWorkerContract) + ". Rebuild the daemon with this Tsugumi revision.";
  if (string_or_empty(target.executor) != "daemon")
    return "Start the Tsugumi remote execution daemon at the registered endpoint.";
  if (string_or_empty(target.isolation) != "pid_namespace")
    return "The execution daemon does not provide PID namespace isolation.";
  const auto* server = schema::get(target.server_id);
  if (!server || !crypto::is_sha256_hex(*server))
    return "The execution daemon did not report a valid server ID.";
  const auto* native = schema::get(target.native_system);
  const auto* systems = schema::get(target.systems);
  if (!native || native->empty() || !systems || std::ranges::find(*systems, *native) == systems->end())
    return "The execution daemon did not report a supported native system.";
  if (!schema::get(target.resource_limits))
    return "The execution daemon did not report its resource limit capability.";
  const auto* max_jobs = schema::get(target.max_jobs);
  if (!max_jobs || *max_jobs == 0 || *max_jobs > std::numeric_limits<uint32_t>::max())
    return "The execution daemon did not report a valid concurrent job limit.";
  return {};
}

}  // namespace

struct RemoteJobClient::Reply {
  schema::Opt<"version", schema::v::Uint64> version;
  schema::Opt<"ok", schema::v::Bool> ok;
  schema::Opt<"error", schema::v::String> error;
  schema::Opt<"code", schema::v::String> code;
  schema::Opt<"sha256", schema::v::String> sha256;
  schema::Opt<"size", schema::v::Uint64> size;
  schema::Opt<"services", schema::v::Array<schema::Obj<TargetJson>>> services;
  schema::Opt<"server_id", schema::v::String> server_id;
  schema::Opt<"instance_id", schema::v::String> instance_id;
  schema::Opt<"worker_contract", schema::v::Uint64> worker_contract;
  schema::Opt<"executor", schema::v::String> executor;
  schema::Opt<"isolation", schema::v::String> isolation;
  schema::Opt<"resource_limits", schema::v::Bool> resource_limits;
  schema::Opt<"max_jobs", schema::v::Uint64> max_jobs;
  schema::Opt<"native_system", schema::v::String> native_system;
  schema::Opt<"systems", schema::v::Array<schema::v::String>> systems;
  schema::Opt<"state", schema::v::String> state;
  schema::Opt<"fingerprint", schema::v::String> fingerprint;
  schema::Opt<"result_sha256", schema::v::String> result_sha256;
  schema::Opt<"diagnostic", schema::v::String> diagnostic;

  auto job() const -> RemoteJobState {
    return {string_or_empty(state), string_or_empty(fingerprint), string_or_empty(result_sha256),
            string_or_empty(diagnostic)};
  }
};

RemoteJobClient::RemoteJobClient()
    : endpoint_(environment("TSUGUMI_EXECUTION_BROKER")),
      token_file_(environment("TSUGUMI_EXECUTION_BROKER_TOKEN_FILE")) {}

namespace {

auto connect_broker(std::string_view endpoint, [[maybe_unused]] bool has_token, const BrokerIo& io,
                    Clock::time_point deadline) -> int {
  if (endpoint.starts_with("unix:")) {
    const std::string_view path = endpoint.substr(5);
    sockaddr_un address{};
    if (path.empty() || path.front() != '/' || path.size() >= sizeof address.sun_path)
      throw BrokerDisconnected("invalid execution broker Unix socket path");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    return socket_for(AF_UNIX, reinterpret_cast<const sockaddr*>(&address), sizeof address, io, deadline);
  }
#if defined(__linux__)
  if (endpoint.starts_with("vsock:")) {
    const std::string_view text = endpoint.substr(6);
    uint32_t port = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (ec != std::errc{} || end != text.data() + text.size() || port == 0 || !has_token)
      throw BrokerDisconnected("invalid authenticated execution broker vsock endpoint");
    sockaddr_vm address{};
    address.svm_family = AF_VSOCK;
    address.svm_cid = VMADDR_CID_HOST;
    address.svm_port = port;
    return socket_for(AF_VSOCK, reinterpret_cast<const sockaddr*>(&address), sizeof address, io, deadline);
  }
#endif
  throw BrokerDisconnected("no execution broker endpoint is configured");
}

}  // namespace

auto RemoteJobClient::request(std::string_view operation, std::string_view service, std::string_view server_id,
                              std::string_view instance, std::string_view job_id, std::string_view digest, uint64_t size, std::istream* body,
                              std::ostream* destination, uint64_t limit, const std::atomic<bool>* cancel) const -> Reply {
  const bool uploading = operation == "put_blob";
  const BrokerIo io{stopped_, cancel, uploading};
  const auto start = Clock::now();
  // Control frames have a fixed deadline even if the peer trickles bytes.
  // Blob bodies can take longer, provided the socket keeps making progress.
  const auto deadline = start + ((uploading || destination) ? kTransferTimeout : kControlTimeout);
  Connection socket{connect_broker(endpoint_, !token_file_.empty(), io, start + kConnectTimeout)};
  const auto version = schema::capture(std::to_string(kProtocolVersion));
  const auto name = schema::string_value(std::string(operation));
  const auto id = schema::string_value(std::string(service));
  const auto server = schema::string_value(std::string(server_id));
  const auto identity = schema::string_value(std::string(instance));
  const auto job = schema::string_value(std::string(job_id));
  const auto hash = schema::string_value(std::string(digest));
  const auto length = schema::capture(std::to_string(size));
  std::vector<std::pair<std::string, const hosq::json::GenericDom<>*>> fields{{"version", &version}, {"operation", &name}};
  if (!service.empty()) fields.emplace_back("service", &id);
  if (!server_id.empty()) fields.emplace_back("server_id", &server);
  if (!instance.empty()) fields.emplace_back("instance_id", &identity);
  if (!job_id.empty()) fields.emplace_back("job_id", &job);
  if (!digest.empty()) fields.emplace_back(operation == "cancel" ? "fingerprint" : "sha256", &hash);
  if (body) fields.emplace_back("size", &length);
  hosq::json::GenericDom<> token;
  if (!token_file_.empty()) {
    auto value = fs::read_file(token_file_);
    if (!value || value->empty() || value->size() > 256)
      throw BrokerDisconnected("cannot read the execution broker token");
    while (!value->empty() && (value->back() == '\r' || value->back() == '\n')) value->pop_back();
    token = schema::string_value(*value);
    fields.emplace_back("broker_token", &token);
  }
  const std::string header = schema::object_text(fields);
  if (header.size() > kMaxHeaderBytes) throw std::runtime_error("execution broker request header exceeds its limit");
  const uint32_t wire_size = htonl(static_cast<uint32_t>(header.size()));
  const auto request_deadline = std::min(deadline, Clock::now() + kControlTimeout);
  send_exact(socket.fd, {reinterpret_cast<const char*>(&wire_size), sizeof wire_size}, io, request_deadline);
  send_exact(socket.fd, header, io, request_deadline);
  std::array<char, 64 * 1024> buffer{};
  while (body && size > 0) {
    io.check(deadline);
    const auto n = static_cast<std::streamsize>(std::min<uint64_t>(size, buffer.size()));
    body->read(buffer.data(), n);
    if (body->gcount() != n) throw std::runtime_error("execution broker request body is truncated");
    send_exact(socket.fd, {buffer.data(), static_cast<size_t>(n)}, io, deadline);
    size -= static_cast<uint64_t>(n);
  }
  // Close the request stream while leaving the broker response readable.
  ::shutdown(socket.fd, SHUT_WR);
  uint32_t response_size = 0;
  const auto response_deadline = std::min(deadline, Clock::now() + kControlTimeout);
  read_exact(socket.fd, reinterpret_cast<char*>(&response_size), sizeof response_size, io, response_deadline);
  response_size = ntohl(response_size);
  if (response_size == 0 || response_size > kMaxHeaderBytes)
    throw BrokerDisconnected("invalid execution broker response header length");
  std::string response(response_size, '\0');
  read_exact(socket.fd, response.data(), response.size(), io, response_deadline);
  Reply reply;
  const auto invalid = schema::decode(response, reply);
  const auto* protocol = schema::get(reply.version);
  const auto* ok = schema::get(reply.ok);
  if (invalid || !protocol || *protocol != kProtocolVersion || !ok)
    throw BrokerDisconnected("invalid execution broker response");
  if (!*ok) {
    const auto code = string_or_empty(reply.code);
    if (code == "instance_changed") throw BrokerInstanceChanged(string_or_empty(reply.error));
    if (code == "registration_changed") throw BrokerDisconnected(string_or_empty(reply.error));
    if (code == "request_rejected") throw BrokerRequestRejected(string_or_empty(reply.error));
    throw std::runtime_error("execution broker: " + string_or_empty(reply.error));
  }
  const auto* incoming = schema::get(reply.size);
  if (destination) {
    if (!incoming || *incoming > limit) throw std::runtime_error("execution broker blob exceeds its size limit");
    crypto::Sha256 hash_check;
    uint64_t remaining = *incoming;
    while (remaining > 0) {
      const size_t n = std::min<uint64_t>(remaining, buffer.size());
      read_exact(socket.fd, buffer.data(), n, io, deadline);
      hash_check.update(buffer.data(), n);
      destination->write(buffer.data(), static_cast<std::streamsize>(n));
      if (!*destination) throw std::runtime_error("cannot store execution broker blob");
      remaining -= n;
    }
    if (hash_check.finish_hex() != digest) throw std::runtime_error("execution broker blob failed SHA-256 verification");
  } else if (incoming && *incoming != 0) {
    throw BrokerDisconnected("unexpected execution broker response body");
  }
  return reply;
}

auto RemoteJobClient::catalog() const -> std::vector<RemoteTarget> {
  if (!enabled()) return {};
  auto reply = request("catalog");
  std::vector<RemoteTarget> result;
  if (const auto* services = schema::get(reply.services)) {
    for (const auto& service : *services) {
      const auto* id = schema::get(service.id);
      if (!id || id->empty()) continue;
      RemoteTarget target{.id = *id, .native_system = string_or_empty(service.native_system),
                          .systems = {}, .server_id = string_or_empty(service.server_id)};
      if (const auto* systems = schema::get(service.systems)) target.systems = *systems;
      if (daemon_capability_error(service).empty()) {
        const auto* available = schema::get(service.available);
        target.available = available && *available;
        target.resource_limits = *schema::get(service.resource_limits);
        target.max_jobs = static_cast<uint32_t>(*schema::get(service.max_jobs));
      }
      result.push_back(std::move(target));
    }
  }
  return result;
}

auto RemoteJobClient::probe(std::string_view service, std::string_view registered_server) const -> RemoteProbe {
  auto reply = request("probe", service, registered_server);
  if (auto error = daemon_capability_error(reply); !error.empty())
    throw Failed(FailureKind::unsupported, std::move(error));
  auto server_id = string_or_empty(reply.server_id);
  if (server_id != registered_server)
    throw Failed(FailureKind::unsupported, "The execution daemon's server ID changed. Verify the registered endpoint.");
  auto instance = string_or_empty(reply.instance_id);
  if (!crypto::is_sha256_hex(instance)) throw BrokerDisconnected("execution daemon did not identify its project journal");
  return {std::move(instance), std::move(server_id), *schema::get(reply.systems), *schema::get(reply.resource_limits)};
}

auto RemoteJobClient::put_blob(std::string_view service, std::string_view server_id, std::string_view instance,
                               std::string_view digest, uint64_t size, std::istream& contents,
                               const std::atomic<bool>* cancel) const -> void {
  request("put_blob", service, server_id, instance, {}, digest, size, &contents, nullptr, 0, cancel);
}

auto RemoteJobClient::get_blob(std::string_view service, std::string_view server_id, std::string_view instance,
                               std::string_view digest, std::ostream& destination, uint64_t limit,
                               const std::atomic<bool>* stop) const -> void {
  request("get_blob", service, server_id, instance, {}, digest, 0, nullptr, &destination, limit, stop);
}

auto RemoteJobClient::submit(std::string_view service, std::string_view server_id, std::string_view instance, std::string_view job_id,
                             std::string_view spec) const -> RemoteJobState {
  std::istringstream body{std::string(spec)};
  return request("submit", service, server_id, instance, job_id, {}, spec.size(), &body).job();
}

auto RemoteJobClient::status(std::string_view service, std::string_view server_id, std::string_view instance,
                             std::string_view job_id) const -> RemoteJobState {
  return request("status", service, server_id, instance, job_id).job();
}

auto RemoteJobClient::cancel(std::string_view service, std::string_view server_id, std::string_view instance, std::string_view job_id,
                             std::string_view fingerprint) const -> RemoteJobState {
  return request("cancel", service, server_id, instance, job_id, fingerprint).job();
}

}  // namespace tsugumi
