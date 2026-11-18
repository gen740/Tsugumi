#include "host/execution_broker.hh"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
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
#include "tsugumi/rpc/peer_tls.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/sqlite.hh"
#include "tsugumi/util/text.hh"
#include "tsugumi/workerd/protocol.hh"

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
auto Configure(int fd) -> void { peer_tls::ConfigureSocket(fd); }
auto ValidId(const std::string& id) -> bool {
  return id != "local" && id != "auto" &&
         std::regex_match(id, std::regex("[a-z][a-z0-9-]{0,31}"));
}
struct Address {
  sockaddr_storage storage{};
  socklen_t size = 0;
};
auto ParseEndpoint(const std::string& endpoint, bool resolve = true)
    -> Address {
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
    Require(
        std::regex_match(
            endpoint, match,
            std::regex(
                R"(tls://(\[[0-9a-fA-F:]+\]|[A-Za-z0-9.-]+):([0-9]{1,5}))")),
        "Enter tls://worker.example:8765");
    auto host = match[1].str();
    if (host.front() == '[') host = host.substr(1, host.size() - 2);
    if (!resolve) return result;
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* addresses = nullptr;
    Require(::getaddrinfo(host.c_str(), match[2].str().c_str(), &hints,
                          &addresses) == 0 &&
                addresses,
            "Cannot resolve remote worker address");
    struct Free {
      addrinfo* p;
      ~Free() { ::freeaddrinfo(p); }
    } free{addresses};
    std::memcpy(&result.storage, addresses->ai_addr, addresses->ai_addrlen);
    result.size = static_cast<socklen_t>(addresses->ai_addrlen);
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
  std::string id, name, endpoint, worker_public_key;
  R::CapabilitiesT capabilities;
  uint64_t grade = 0;
  // Only transport identity changes invalidate a project's grant. Capabilities
  // are observations of the worker and the name is presentation.
  std::string generation;
};
auto Public(const Entry& entry) -> std::unique_ptr<H::ExecutionServiceT> {
  auto value = std::make_unique<H::ExecutionServiceT>();
  value->id = entry.id;
  value->name = entry.name;
  value->endpoint = entry.endpoint;
  value->worker_public_key = entry.worker_public_key;
  value->grade = entry.grade;
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
auto VerifiedCapabilities(const R::ReplyT& reply) -> const R::CapabilitiesT& {
  const auto* capabilities = reply.body.AsCapabilities();
  if (!capabilities || capabilities->worker_contract != W::kContract ||
      !crypto::is_sha256_hex(capabilities->server_id) ||
      !capabilities->max_jobs)
    throw W::Error(R::ErrorCode::Rejected,
                   "Rebuild the remote worker with this Tsugumi revision "
                   "(invalid capabilities)");
  return *capabilities;
}
}  // namespace

struct ExecutionBroker::State {
  std::filesystem::path state;
  std::string socket, key;
  sql::Db db;
  peer_tls::Identity identity;
  std::mutex mutex;
  std::set<int> active;
  bool stopping = false;
  Socket listener;
  std::vector<std::jthread> threads;

  State(std::filesystem::path dir, std::string path)
      : state(std::move(dir)),
        socket(std::move(path)),
        db((state / "execution.sqlite").string()),
        identity(state / "execution-identity.pem") {
    ::chmod((state / "execution.sqlite").c_str(), 0600);
    db.exec(
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS services(id TEXT PRIMARY KEY,name TEXT NOT "
        "NULL,"
        "endpoint TEXT NOT NULL,worker_public_key TEXT NOT NULL,capabilities "
        "BLOB NOT "
        "NULL,grade TEXT NOT NULL,generation TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS projects(id TEXT PRIMARY KEY,identity TEXT "
        "NOT NULL,"
        "scope TEXT NOT NULL,capability TEXT NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS project_services(project TEXT NOT NULL,"
        "service TEXT NOT NULL,generation TEXT NOT NULL,PRIMARY "
        "KEY(project,service))");
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
        "SELECT id,name,endpoint,worker_public_key,capabilities,"
        "grade,generation FROM services "
        "ORDER BY (id != 'local'),rowid");
    std::vector<Entry> entries;
    while (q.step()) {
      const auto bytes = q.blob(4);
      Require(bytes.size() <= W::kMaxFrame,
              "Invalid stored worker capabilities");
      auto message = W::decode(bytes);
      const auto* reply = message->payload.AsReply();
      Require(reply && reply->body.AsCapabilities(),
              "Invalid stored worker capabilities");
      entries.push_back({q.text(0), q.text(1), q.text(2), q.text(3),
                         *reply->body.AsCapabilities(), std::stoull(q.text(5)),
                         q.text(6)});
    }
    if (std::ranges::find(entries, "local", &Entry::id) == entries.end()) {
      Entry local;
      local.id = "local";
      local.name = "local";
      local.capabilities.server_id = "";
      local.capabilities.native_system = "local";
      local.capabilities.systems = {"local"};
      entries.insert(entries.begin(), std::move(local));
    }
    return entries;
  }
  auto List() -> H::ExecutionServicesT {
    H::ExecutionServicesT result;
    result.host_public_key = identity.public_key();
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
      int ready;
      do {
        ready = ::poll(&p, 1, -1);
      } while (ready < 0 && errno == EINTR);
      connected =
          ready > 0 &&
          ::getsockopt(peer.fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
          !error;
    }
    Require(connected, "Cannot connect to remote worker");
    ::fcntl(peer.fd, F_SETFL, 0);
    peer_tls::Context context(identity, false, [&](std::string_view key) {
      return key == entry.worker_public_key;
    });
    peer_tls::Stream tls(context, peer.fd, false);
    R::MessageT message;
    message.payload.Set(std::move(request));
    auto bytes = W::encode(std::move(message));
    tls.Write(W::length_prefix(static_cast<uint32_t>(bytes.size())) + bytes);
    const auto length = W::frame_length(tls.Read(4));
    auto reply = W::decode(tls.Read(length));
    Require(reply->payload.AsReply(), "Expected remote worker reply");
    return std::move(*reply);
  }
  auto Forward(int fd) -> void {
    bool forwarding = false;
    // A framing/verification failure has no trusted request to answer.
    auto message = Read(fd);
    try {
      auto* broker = message->payload.AsBrokerRequest();
      Require(broker, "Expected a private broker envelope");
      auto* request = broker->request.get();
      Entry entry;
      std::optional<R::CatalogT> catalog_reply;
      {
        std::lock_guard lock(mutex);
        auto q = db.prepare("SELECT scope,id FROM projects WHERE capability=?");
        q.bind(1, broker->capability);
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
            target->grade = e.grade;
            auto grant = db.prepare(
                "SELECT generation FROM project_services "
                "WHERE project=? AND service=?");
            grant.bind(1, q.text(1)).bind(2, e.id);
            const bool approved =
                e.id == "local" ||
                (grant.step() && grant.text(0) == e.generation);
            target->available = e.id == "local" ||
                                (approved && !e.capabilities.server_id.empty());
            if (!target->available)
              target->unavailable_reason =
                  "Worker " + e.id +
                  (approved ? " needs its registration verified in settings"
                            : " needs a person's approval for this project");
            target->capabilities =
                std::make_unique<R::CapabilitiesT>(e.capabilities);
            catalog.services.push_back(std::move(target));
          }
          catalog_reply = std::move(catalog);
        } else {
          const auto found =
              std::ranges::find(entries, request->service, &Entry::id);
          if (found == entries.end() || found->id == "local" ||
              found->capabilities.server_id.empty())
            throw W::Error(
                R::ErrorCode::RegistrationChanged,
                "Verify the execution service registration in settings");
          auto grant = db.prepare(
              "SELECT generation FROM project_services "
              "WHERE project=? AND service=?");
          grant.bind(1, q.text(1)).bind(2, found->id);
          if (!grant.step() || grant.text(0) != found->generation)
            throw W::Error(R::ErrorCode::RegistrationChanged,
                           "Execution service registration changed; "
                           "reauthorize this project");
          entry = *found;
        }
      }
      if (catalog_reply) {
        Write(fd, W::reply(std::move(*catalog_reply)));
        return;
      }
      forwarding = true;
      const bool probing = request->body.AsProbeRequest() != nullptr;
      auto response = Remote(entry, std::move(*request));
      if (probing) {
        const auto& reply = *response.payload.AsReply();
        if (!reply.body.AsFailure()) {
          auto cached = VerifiedCapabilities(reply);
          if (cached.server_id != entry.capabilities.server_id)
            throw W::Error(R::ErrorCode::Rejected,
                           "The remote worker's server ID changed. Verify the "
                           "registered endpoint.");
          if (!crypto::is_sha256_hex(cached.instance_id))
            throw W::Error(
                R::ErrorCode::Rejected,
                "The remote worker did not identify its project journal");
          cached.instance_id.clear();
          std::lock_guard lock(mutex);
          auto update = db.prepare(
              "UPDATE services SET capabilities=? WHERE id=? AND generation=?");
          update.bind_blob(1, W::encode(W::reply(std::move(cached))))
              .bind(2, entry.id)
              .bind(3, entry.generation)
              .run();
        }
      }
      Write(fd, std::move(response));
    } catch (const std::exception& e) {
      // A transport interruption after forwarding leaves submit unconfirmed.
      const auto* worker_error = dynamic_cast<const W::Error*>(&e);
      if (forwarding && !worker_error) return;
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
auto ExecutionBroker::socket_path() const -> const std::string& {
  return state_->socket;
}
auto ExecutionBroker::Authorize(const std::string& project,
                                const std::filesystem::path& path,
                                const std::filesystem::path& control,
                                bool guest) -> H::ExecutionAuthorizationT {
  std::lock_guard lock(state_->mutex);
  const auto identity = ProjectIdentity(path.string());
  Require(!project.empty(), "Missing project identity");
  Require(control.is_absolute() && std::filesystem::is_directory(control),
          "Invalid project control directory");
  if (identity.empty()) {
    auto q = state_->db.prepare("DELETE FROM projects WHERE id=?");
    q.bind(1, project).run();
    state_->db.prepare("DELETE FROM project_services WHERE project=?")
        .bind(1, project)
        .run();
    fs::remove_file_durable(control / "execution-broker-capability");
    H::ExecutionAuthorizationT result;
    result.endpoint = "unix:" + socket_path();
    result.capability_file = "";
    return result;
  }
  const auto scope = Tag(state_->key, std::string("project\0", 8) + identity);
  auto capability = crypto::random_token(32);
  sql::Transaction transaction(state_->db);
  auto previous =
      state_->db.prepare("SELECT capability,identity FROM projects WHERE id=?");
  previous.bind(1, project);
  if (previous.step() && previous.text(1) == identity)
    capability = previous.text(0);
  auto q =
      state_->db.prepare("INSERT OR REPLACE INTO projects VALUES(?,?,?,?)");
  q.bind(1, project).bind(2, identity).bind(3, scope).bind(4, capability).run();
  state_->db.prepare("DELETE FROM project_services WHERE project=?")
      .bind(1, project)
      .run();
  for (const auto& entry : state_->Entries()) {
    auto grant =
        state_->db.prepare("INSERT INTO project_services VALUES(?,?,?)");
    grant.bind(1, project).bind(2, entry.id).bind(3, entry.generation).run();
  }
  transaction.commit();
  fs::write_atomic(control / "execution-broker-capability", capability,
                   guest ? 0644 : 0600);
  H::ExecutionAuthorizationT result;
  result.endpoint = "unix:" + socket_path();
  result.capability_file = (control / "execution-broker-capability").string();
  result.available = true;
  return result;
}
auto ExecutionBroker::Revoke(const std::string& project) -> void {
  std::lock_guard lock(state_->mutex);
  auto q = state_->db.prepare("DELETE FROM projects WHERE id=?");
  q.bind(1, project).run();
  state_->db.prepare("DELETE FROM project_services WHERE project=?")
      .bind(1, project)
      .run();
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
    if (const auto* p = request.value_as_ExecutionProjectAuthorizeRequest())
      return agent::wire::HostReply(
          id, Authorize(p->project_id()->str(), p->path()->str(),
                        p->control_dir()->str(), p->guest()));
    if (const auto* p = request.value_as_ExecutionProjectRevokeRequest()) {
      Revoke(p->project_id()->str());
      return agent::wire::HostReply(id, Tsugumi::Service::AcknowledgementT{});
    }
    if (const auto* p = request.value_as_ExecutionServicePutRequest()) {
      Entry entry{p->id()->str(),
                  p->name()->str(),
                  p->endpoint()->str(),
                  p->worker_public_key()->str(),
                  {},
                  p->grade(),
                  crypto::random_token(32)};
      Require(entry.id == "local" || ValidId(entry.id),
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
      Require(entry.id != "local" ||
                  (entry.endpoint.empty() && entry.worker_public_key.empty()),
              "The local worker has no endpoint or public key");
      if (entry.id != "local") ParseEndpoint(entry.endpoint, false);
      std::string previous;
      {
        std::lock_guard lock(state_->mutex);
        for (const auto& e : state_->Entries())
          if (e.id == entry.id) {
            previous = e.generation + e.endpoint + e.worker_public_key +
                       e.name + "\n" + std::to_string(e.grade);
            if (entry.endpoint == e.endpoint &&
                entry.worker_public_key == e.worker_public_key)
              entry.capabilities = e.capabilities;
            entry.generation = e.generation;
          }
      }
      if (entry.id != "local") peer_tls::ValidateKey(entry.worker_public_key);
      if (entry.id != "local" && p->verify()) {
        R::RequestT probe;
        probe.body.Set(R::ProbeRequestT{});
        auto response = state_->Remote(entry, std::move(probe));
        auto* reply = response.payload.AsReply();
        if (reply->body.AsFailure())
          throw std::runtime_error(
              "The remote worker rejected the registration");
        entry.capabilities = VerifiedCapabilities(*reply);
        entry.capabilities.instance_id.clear();
      }
      std::lock_guard lock(state_->mutex);
      std::string current;
      for (const auto& e : state_->Entries())
        if (e.id == entry.id) {
          current = e.generation + e.endpoint + e.worker_public_key + e.name +
                    "\n" + std::to_string(e.grade);
          if (e.endpoint != entry.endpoint ||
              e.worker_public_key != entry.worker_public_key ||
              e.capabilities.server_id != entry.capabilities.server_id)
            entry.generation = crypto::random_token(32);
          else if (!p->verify())
            entry.capabilities = e.capabilities;
        }
      const auto caps = W::encode(W::reply(entry.capabilities));
      Require(current == previous,
              "Registration changed while verifying; reopen settings");
      sql::Transaction transaction(state_->db);
      auto q = state_->db.prepare(
          "INSERT INTO services VALUES(?,?,?,?,?,?,?) ON CONFLICT(id) DO "
          "UPDATE SET "
          "name=excluded.name,endpoint=excluded.endpoint,worker_public_key="
          "excluded.worker_public_key,capabilities=excluded.capabilities,grade="
          "excluded.grade,generation=excluded.generation");
      q.bind(1, entry.id)
          .bind(2, entry.name)
          .bind(3, entry.endpoint)
          .bind(4, entry.worker_public_key)
          .bind_blob(5, caps)
          .bind(6, std::to_string(entry.grade))
          .bind(7, entry.generation)
          .run();
      transaction.commit();
      return agent::wire::HostReply(id, state_->List());
    }
    std::lock_guard lock(state_->mutex);
    if (const auto* p = request.value_as_ExecutionServiceRemoveRequest()) {
      Require(ValidId(p->id()->str()), "Invalid execution service ID");
      sql::Transaction transaction(state_->db);
      auto q = state_->db.prepare("DELETE FROM services WHERE id=?");
      q.bind(1, p->id()->str()).run();
      transaction.commit();
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
