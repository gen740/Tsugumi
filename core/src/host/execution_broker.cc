#include "host/execution_broker.hh"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/stat.h>
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <thread>

#include "agent/wire.hh"
#include "tsugumi/workerd/protocol.hh"
#include "util/crypto.hh"
#include "util/fs.hh"
#include "util/sqlite.hh"
#include "util/text.hh"

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace R = Tsugumi::RemoteWorker;
namespace W = workerd;
namespace {
struct Socket {
  int fd = -1;
  ~Socket() {
    if (fd >= 0) ::close(fd);
  }
};
auto Require(bool ok, const char* message) -> void {
  if (!ok) throw std::runtime_error(message);
}
auto Exact(int fd, std::string& bytes, bool write) -> void {
  size_t offset = 0;
  while (offset < bytes.size()) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto n =
        write ? ::send(fd, bytes.data() + offset, bytes.size() - offset, flags)
              : ::recv(fd, bytes.data() + offset, bytes.size() - offset, 0);
    if (n < 0 && errno == EINTR) continue;
    Require(n > 0, "Execution stream disconnected or timed out");
    offset += static_cast<size_t>(n);
  }
}
auto Read(int fd) -> std::unique_ptr<R::MessageT> {
  std::string prefix(4, '\0');
  Exact(fd, prefix, false);
  std::string bytes(W::frame_length(prefix), '\0');
  Exact(fd, bytes, false);
  return W::decode(bytes);
}
auto Write(int fd, R::MessageT message) -> void {
  auto bytes = W::encode(std::move(message));
  auto prefix = W::length_prefix(static_cast<uint32_t>(bytes.size()));
  Exact(fd, prefix, true);
  Exact(fd, bytes, true);
}
auto Configure(int fd) -> void {
  Require(::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0,
          "Cannot configure execution socket");
  timeval timeout{60, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
}
auto ValidId(const std::string& id) -> bool {
  return id != "local" &&
         std::regex_match(id, std::regex("[a-z][a-z0-9-]{0,31}"));
}
struct Address {
  sockaddr_storage storage{};
  socklen_t size = 0;
};
auto ParseEndpoint(const std::string& endpoint) -> Address {
  Require(endpoint.size() <= 2048 && endpoint.find('\0') == std::string::npos,
          "Invalid worker endpoint");
  Address result;
  if (endpoint.starts_with("unix:/")) {
    auto* address = reinterpret_cast<sockaddr_un*>(&result.storage);
    const auto path = endpoint.substr(5);
    Require(path.size() < sizeof address->sun_path,
            "Worker socket path is too long");
    address->sun_family = AF_UNIX;
    std::memcpy(address->sun_path, path.c_str(), path.size() + 1);
    result.size = sizeof *address;
  } else {
    std::smatch match;
    Require(std::regex_match(
                endpoint, match,
                std::regex(R"(tcp://(127\.0\.0\.1|\[::1\]):([0-9]{1,5}))")),
            "Enter unix:/absolute/socket or tcp://127.0.0.1:8765");
    const auto port = std::stoul(match[2]);
    Require(port > 0 && port <= 65535, "Invalid worker port");
    if (match[1] == "127.0.0.1") {
      auto* address = reinterpret_cast<sockaddr_in*>(&result.storage);
      address->sin_family = AF_INET;
      address->sin_port = htons(static_cast<uint16_t>(port));
      address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      result.size = sizeof *address;
    } else {
      auto* address = reinterpret_cast<sockaddr_in6*>(&result.storage);
      address->sin6_family = AF_INET6;
      address->sin6_port = htons(static_cast<uint16_t>(port));
      address->sin6_addr = in6addr_loopback;
      result.size = sizeof *address;
    }
  }
  return result;
}
// HMAC-SHA256 uses the platform SHA implementation already linked by core.
auto Tag(std::string_view key, std::string_view value) -> std::string {
  std::array<char, 64> inner{}, outer{};
  for (size_t i = 0; i < inner.size(); ++i) {
    const auto c = i < key.size() ? key[i] : 0;
    inner[i] = static_cast<char>(c ^ 0x36);
    outer[i] = static_cast<char>(c ^ 0x5c);
  }
  crypto::Sha256 a;
  a.update(inner.data(), inner.size());
  a.update(value);
  const auto digest = a.finish();
  crypto::Sha256 b;
  b.update(outer.data(), outer.size());
  b.update(digest.data(), digest.size());
  return b.finish_hex();
}
auto ReadKey(const std::filesystem::path& path) -> std::string {
  Socket file{
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (file.fd < 0 && errno == ENOENT) return {};
  struct stat st{};
  Require(file.fd >= 0 && ::fstat(file.fd, &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_uid == ::getuid() && !(st.st_mode & 0077) &&
              st.st_size == 64,
          "The execution broker key must be a private host file");
  std::string key(64, '\0');
  Require(::read(file.fd, key.data(), key.size()) ==
                  static_cast<ssize_t>(key.size()) &&
              crypto::is_sha256_hex(key),
          "Invalid execution broker key");
  return key;
}
auto Text(const flatbuffers::String* value) -> std::string {
  return value ? value->str() : "";
}
// Birth time distinguishes a replacement directory when an inode is reused.
// A filesystem without birth time can still run local Cards; it gets no remote
// capability. Both clients use this single host-side interpretation.
auto ProjectIdentity(const std::string& path) -> std::string {
  Require(!path.empty() && path.front() == '/' &&
              path.find('\0') == std::string::npos,
          "Invalid project path");
  struct stat st{};
  Require(::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode),
          "Invalid project directory");
  int64_t seconds = 0, nanos = 0;
#ifdef __APPLE__
  seconds = st.st_birthtimespec.tv_sec;
  nanos = st.st_birthtimespec.tv_nsec;
#elif defined(__linux__)
  struct statx extended{};
  if (::syscall(SYS_statx, AT_FDCWD, path.c_str(), 0, STATX_BTIME | STATX_INO,
                &extended) == 0 &&
      (extended.stx_mask & STATX_BTIME) && extended.stx_ino == st.st_ino) {
    seconds = extended.stx_btime.tv_sec;
    nanos = extended.stx_btime.tv_nsec;
  }
#endif
  if (seconds <= 0) return {};
  return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" +
         std::to_string(seconds) + ":" + std::to_string(nanos);
}
struct Entry {
  std::string id, name, endpoint, token;
  R::CapabilitiesT capabilities;
};
auto Public(const Entry& entry) -> std::unique_ptr<H::ExecutionServiceT> {
  auto value = std::make_unique<H::ExecutionServiceT>();
  value->id = entry.id;
  value->name = entry.name;
  value->endpoint = entry.endpoint;
  auto& c = value->capabilities = std::make_unique<H::ExecutionCapabilitiesT>();
  c->isolation = "pid_namespace";
  c->server_id = entry.capabilities.server_id;
  c->native_system = entry.capabilities.native_system;
  c->systems = entry.capabilities.systems;
  c->resource_limits = entry.capabilities.resource_limits;
  c->max_concurrent_jobs = entry.capabilities.max_jobs;
  c->worker_contract = entry.capabilities.worker_contract;
  return value;
}
auto CheckCapabilities(const R::CapabilitiesT* c) -> void {
  Require(
      c && crypto::is_sha256_hex(c->server_id) && c->max_jobs &&
          c->worker_contract == W::kContract &&
          std::ranges::find(c->systems, c->native_system) != c->systems.end() &&
          std::ranges::all_of(c->systems,
                              [](const auto& s) {
                                return s == "aarch64-linux" ||
                                       s == "x86_64-linux";
                              }),
      "Rebuild the remote worker with this Tsugumi revision (invalid "
      "capabilities)");
}
}  // namespace

struct ExecutionBroker::State {
  std::filesystem::path state;
  std::string socket, key;
  sql::Db db;
  std::mutex mutex;
  std::set<int> active;
  bool stopping = false;
  Socket listener;
  std::vector<std::jthread> threads;

  State(std::filesystem::path dir, std::string path)
      : state(std::move(dir)),
        socket(std::move(path)),
        db((state / "execution.sqlite").string()) {
    ::chmod((state / "execution.sqlite").c_str(), 0600);
    db.exec(
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS services(id TEXT PRIMARY KEY,name TEXT NOT "
        "NULL,"
        "endpoint TEXT NOT NULL,token TEXT NOT NULL,capabilities BLOB NOT "
        "NULL);"
        "CREATE TABLE IF NOT EXISTS projects(id TEXT PRIMARY KEY,identity TEXT "
        "NOT NULL,"
        "scope TEXT NOT NULL,token TEXT NOT NULL UNIQUE)");
    const auto file = state / "execution-scope-key";
    key = ReadKey(file);
    if (key.empty()) {
      fs::write_atomic(file, crypto::random_token(32), 0600);
      key = ReadKey(file);
    }
    auto address = ParseEndpoint("unix:" + socket);
    listener.fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    Require(listener.fd >= 0, "Cannot open execution broker listener");
    Configure(listener.fd);
    ::unlink(socket.c_str());
    Require(::bind(listener.fd, reinterpret_cast<sockaddr*>(&address.storage),
                   address.size) == 0 &&
                ::chmod(socket.c_str(), 0600) == 0 &&
                ::listen(listener.fd, 64) == 0,
            "Cannot listen on execution broker socket");
    ::fcntl(listener.fd, F_SETFL, O_NONBLOCK);
    for (int i = 0; i < 8; ++i)
      threads.emplace_back([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
          pollfd p{listener.fd, POLLIN, 0};
          if (::poll(&p, 1, 100) <= 0) continue;
          // Serialize accept with shutdown, and use a nonblocking listener so
          // competing acceptors cannot get stuck after another drained the
          // queue.
          Socket client;
          {
            std::lock_guard lock(mutex);
            if (stopping) return;
            client.fd = ::accept(listener.fd, nullptr, nullptr);
            if (client.fd < 0) continue;
            active.insert(client.fd);
          }
          try {
            Configure(client.fd);
            Forward(client.fd);
          } catch (...) {
          }
          {
            std::lock_guard lock(mutex);
            active.erase(client.fd);
          }
        }
      });
  }
  auto Entries() -> std::vector<Entry> {
    auto q = db.prepare(
        "SELECT id,name,endpoint,token,capabilities FROM services ORDER BY id");
    std::vector<Entry> entries;
    while (q.step()) {
      auto message = W::decode(q.blob(4));
      const auto* reply = message->payload.AsReply();
      Require(reply && reply->body.AsCapabilities(),
              "Invalid stored worker capabilities");
      entries.push_back({q.text(0), q.text(1), q.text(2), q.text(3),
                         *reply->body.AsCapabilities()});
    }
    return entries;
  }
  auto List() -> H::ExecutionServicesT {
    H::ExecutionServicesT result;
    for (const auto& entry : Entries())
      result.services.push_back(Public(entry));
    return result;
  }
  auto Remote(const Entry& entry, R::RequestT request) -> R::MessageT {
    const auto address = ParseEndpoint(entry.endpoint);
    Socket peer{::socket(address.storage.ss_family, SOCK_STREAM, 0)};
    Require(peer.fd >= 0, "Cannot open worker connection");
    Configure(peer.fd);
    ::fcntl(peer.fd, F_SETFL, O_NONBLOCK);
    {
      std::lock_guard lock(mutex);
      Require(!stopping, "Execution broker is stopping");
      active.insert(peer.fd);
    }
    struct Guard {
      State& state;
      int fd;
      ~Guard() {
        std::lock_guard lock(state.mutex);
        state.active.erase(fd);
      }
    } guard{*this, peer.fd};
    auto connected =
        ::connect(peer.fd, reinterpret_cast<const sockaddr*>(&address.storage),
                  address.size) == 0;
    if (!connected && errno == EINPROGRESS) {
      pollfd p{peer.fd, POLLOUT, 0};
      int error = 0;
      socklen_t size = sizeof error;
      connected =
          ::poll(&p, 1, 10000) > 0 &&
          ::getsockopt(peer.fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
          !error;
    }
    Require(connected, "Cannot connect to remote worker");
    ::fcntl(peer.fd, F_SETFL, 0);
    request.token = entry.token;
    R::MessageT message;
    message.payload.Set(std::move(request));
    Write(peer.fd, std::move(message));
    auto reply = Read(peer.fd);
    Require(reply->payload.AsReply(), "Expected remote worker reply");
    return std::move(*reply);
  }
  auto Forward(int fd) -> void {
    bool forwarding = false;
    try {
      auto message = Read(fd);
      auto* request = message->payload.AsRequest();
      Require(request, "Expected execution broker request");
      Entry entry;
      std::optional<R::CatalogT> catalog_reply;
      {
        std::lock_guard lock(mutex);
        auto q = db.prepare("SELECT scope FROM projects WHERE token=?");
        q.bind(1, request->token);
        if (!q.step())
          throw W::Error(R::ErrorCode::Unauthorized,
                         "Project is not authorized to use this broker");
        request->project_scope = q.text(0);
        auto entries = Entries();
        if (request->body.AsCatalogRequest()) {
          R::CatalogT catalog;
          for (const auto& e : entries) {
            auto target = std::make_unique<R::TargetT>();
            target->id = e.id;
            target->available = e.capabilities.worker_contract == W::kContract;
            target->capabilities =
                std::make_unique<R::CapabilitiesT>(e.capabilities);
            catalog.services.push_back(std::move(target));
          }
          catalog_reply = std::move(catalog);
        } else {
          const auto found =
              std::ranges::find(entries, request->service, &Entry::id);
          if (found == entries.end() ||
              found->capabilities.server_id != request->server_id ||
              found->capabilities.worker_contract != W::kContract)
            throw W::Error(
                R::ErrorCode::RegistrationChanged,
                "Verify the execution service registration in settings");
          Require(request->body.type != R::RequestBody::NONE,
                  "Missing worker request");
          entry = *found;
          request->server_id = entry.capabilities.server_id;
        }
      }
      if (catalog_reply) {
        Write(fd, W::reply(std::move(*catalog_reply)));
        return;
      }
      forwarding = true;
      Write(fd, Remote(entry, std::move(*request)));
    } catch (const std::exception& e) {
      // A transport interruption after forwarding leaves submit unconfirmed.
      if (forwarding) return;
      const auto* worker_error = dynamic_cast<const W::Error*>(&e);
      Write(fd, W::failure(
                    worker_error ? worker_error->code : R::ErrorCode::Invalid,
                    e.what()));
    }
  }
  auto Stop() -> void {
    {
      std::lock_guard lock(mutex);
      if (stopping) return;
      stopping = true;
      for (int fd : active) ::shutdown(fd, SHUT_RDWR);
      for (auto& thread : threads) thread.request_stop();
    }
    threads.clear();
    ::unlink(socket.c_str());
  }
};

ExecutionBroker::ExecutionBroker(std::filesystem::path state,
                                 std::string socket)
    : state_(std::make_unique<State>(std::move(state), std::move(socket))) {}
ExecutionBroker::~ExecutionBroker() { Stop(); }
auto ExecutionBroker::Stop() -> void { state_->Stop(); }
auto ExecutionBroker::endpoint() const -> std::string {
  return "unix:" + state_->socket;
}
auto ExecutionBroker::Handles(H::RequestPayload type) -> bool {
  return type == H::RequestPayload::ExecutionServicesRequest ||
         type == H::RequestPayload::ExecutionServicePutRequest ||
         type == H::RequestPayload::ExecutionServiceRemoveRequest ||
         type == H::RequestPayload::ExecutionProjectAuthorizeRequest ||
         type == H::RequestPayload::ExecutionProjectRevokeRequest;
}
auto ExecutionBroker::Handle(const H::Request& request, uint64_t id)
    -> std::string {
  try {
    if (const auto* p = request.value_as_ExecutionServicePutRequest()) {
      Entry entry{p->id()->str(),
                  p->name()->str(),
                  p->endpoint()->str(),
                  Text(p->token()),
                  {}};
      Require(ValidId(entry.id),
              "Invalid execution service ID (local is reserved)");
      Require(
          text::valid_utf8(entry.name) &&
              std::ranges::count_if(
                  entry.name,
                  [](unsigned char c) { return (c & 0xc0) != 0x80; }) <= 80 &&
              entry.name.find_first_not_of(" \t\r\n") != std::string::npos &&
              std::ranges::none_of(
                  entry.name,
                  [](unsigned char c) { return c < 32 || c == 127; }),
          "Enter a service name of up to 80 characters");
      ParseEndpoint(entry.endpoint);
      std::string previous;
      {
        std::lock_guard lock(state_->mutex);
        for (const auto& e : state_->Entries())
          if (e.id == entry.id) {
            previous = W::encode(W::reply(e.capabilities)) + e.endpoint +
                       e.token + e.name;
            if (entry.token.empty() && entry.endpoint == e.endpoint)
              entry.token = e.token;
          }
      }
      Require(crypto::is_sha256_hex(entry.token),
              "Enter the worker's 64-character lowercase hex token");
      R::RequestT probe;
      probe.body.Set(R::ProbeRequestT{});
      auto response = state_->Remote(entry, std::move(probe));
      auto* reply = response.payload.AsReply();
      if (reply->body.AsFailure())
        throw std::runtime_error("The remote worker rejected the registration");
      CheckCapabilities(reply->body.AsCapabilities());
      entry.capabilities = *reply->body.AsCapabilities();
      // An instance belongs to a live probe; registrations persist only server
      // identity.
      entry.capabilities.instance_id.clear();
      const auto caps = W::encode(W::reply(entry.capabilities));
      std::lock_guard lock(state_->mutex);
      std::string current;
      for (const auto& e : state_->Entries())
        if (e.id == entry.id)
          current = W::encode(W::reply(e.capabilities)) + e.endpoint + e.token +
                    e.name;
      Require(current == previous,
              "Registration changed while verifying; reopen settings");
      auto q = state_->db.prepare(
          "INSERT OR REPLACE INTO services VALUES(?,?,?,?,?)");
      q.bind(1, entry.id)
          .bind(2, entry.name)
          .bind(3, entry.endpoint)
          .bind(4, entry.token)
          .bind_blob(5, caps)
          .run();
      return agent::wire::HostReply(id, state_->List());
    }
    std::lock_guard lock(state_->mutex);
    if (const auto* p = request.value_as_ExecutionServiceRemoveRequest()) {
      Require(ValidId(p->id()->str()), "Invalid execution service ID");
      auto q = state_->db.prepare("DELETE FROM services WHERE id=?");
      q.bind(1, p->id()->str()).run();
    }
    if (const auto* p = request.value_as_ExecutionProjectAuthorizeRequest()) {
      const auto identity = ProjectIdentity(p->path()->str()),
                 project = p->project_id()->str();
      Require(!project.empty(), "Missing project identity");
      const std::filesystem::path control(p->control_dir()->str());
      Require(control.is_absolute() && std::filesystem::is_directory(control),
              "Invalid project control directory");
      if (identity.empty()) {
        auto q = state_->db.prepare("DELETE FROM projects WHERE id=?");
        q.bind(1, project).run();
        fs::remove_file_durable(control / "execution-broker-token");
        H::ExecutionAuthorizationT result;
        result.endpoint = endpoint();
        result.token_file = "";
        return agent::wire::HostReply(id, std::move(result));
      }
      const auto scope =
          Tag(state_->key, std::string("project\0", 8) + identity);
      auto token = crypto::random_token(32);
      auto previous =
          state_->db.prepare("SELECT token,identity FROM projects WHERE id=?");
      previous.bind(1, project);
      if (previous.step() && previous.text(1) == identity)
        token = previous.text(0);
      auto q =
          state_->db.prepare("INSERT OR REPLACE INTO projects VALUES(?,?,?,?)");
      q.bind(1, project).bind(2, identity).bind(3, scope).bind(4, token).run();
      fs::write_atomic(control / "execution-broker-token", token,
                       p->guest() ? 0644 : 0600);
      H::ExecutionAuthorizationT result;
      result.endpoint = endpoint();
      result.token_file = (control / "execution-broker-token").string();
      result.available = true;
      return agent::wire::HostReply(id, std::move(result));
    }
    if (const auto* p = request.value_as_ExecutionProjectRevokeRequest()) {
      auto q = state_->db.prepare("DELETE FROM projects WHERE id=?");
      q.bind(1, p->project_id()->str()).run();
      return agent::wire::HostReply(id, Tsugumi::Service::AcknowledgementT{});
    }
    return agent::wire::HostReply(id, state_->List());
  } catch (const std::exception& e) {
    Tsugumi::Service::FailureT failure;
    failure.code = "invalid_request";
    failure.message = e.what();
    return agent::wire::HostReply(id, std::move(failure));
  }
}
}  // namespace tsugumi::host
