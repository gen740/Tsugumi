#include "host/execution_broker.hh"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "tsugumi/protocol/trace_codec.hh"
#ifdef __linux__
#include <linux/stat.h>
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <thread>
#include <utility>

#include "agent/wire.hh"
#include "tsugumi/rpc/peer_tls.hh"
#include "tsugumi/rpc/unix_binding.hh"
#include "tsugumi/rpc/unix_socket.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/util/sqlite.hh"
#include "tsugumi/util/text.hh"
#include "tsugumi/workerd/log_limits.hh"
#include "tsugumi/workerd/protocol.hh"

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace R = Tsugumi::RemoteWorker;
namespace W = workerd;
namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
constexpr auto kExchangeTimeout = std::chrono::seconds(30);
constexpr size_t kTransferSlots = 8;
constexpr size_t kPendingTransfers = 64;
constexpr size_t kPendingBytes = 64u << 20;

struct Socket {
  explicit Socket(int value = -1) : fd(value) {}
  Socket(const Socket&) = delete;
  Socket(Socket&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
  int fd = -1;
  ~Socket() {
    if (fd >= 0) ::close(fd);
  }
};
auto Require(bool ok, const char* message) -> void {
  if (!ok) throw std::runtime_error(message);
}
auto Remaining(Deadline deadline) -> int {
  const auto left =
      std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
  Require(left.count() > 0, "Execution stream timed out");
  return static_cast<int>(left.count());
}
auto WaitForSocket(int fd, short events, Deadline deadline, int client = -1)
    -> void {
  pollfd sockets[]{{fd, events, 0}, {client, POLLIN, 0}};
  int ready;
  do {
    ready = ::poll(sockets, 2, Remaining(deadline));
  } while (ready < 0 && errno == EINTR);
  Require(ready != 0, "Execution stream timed out");
  Require(ready > 0 && !sockets[1].revents, "Execution stream disconnected");
}
auto Exact(int fd, std::string& bytes, bool write, Deadline deadline) -> void {
  size_t offset = 0;
  while (offset < bytes.size()) {
    (void)Remaining(deadline);
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto n =
        write ? ::send(fd, bytes.data() + offset, bytes.size() - offset, flags)
              : ::recv(fd, bytes.data() + offset, bytes.size() - offset, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      WaitForSocket(fd, write ? POLLOUT : POLLIN, deadline);
      continue;
    }
    Require(n > 0, "Execution stream disconnected");
    offset += static_cast<size_t>(n);
  }
}
auto Read(int fd, Deadline deadline) -> std::string {
  std::string prefix(4, '\0');
  Exact(fd, prefix, false, deadline);
  std::string bytes(W::frame_length(prefix), '\0');
  Exact(fd, bytes, false, deadline);
  return bytes;
}
auto Write(int fd, R::MessageT message, Deadline deadline) -> void {
  auto bytes = W::encode(std::move(message));
  auto prefix = W::length_prefix(static_cast<uint32_t>(bytes.size()));
  Exact(fd, prefix, true, deadline);
  Exact(fd, bytes, true, deadline);
}
auto Configure(int fd) -> void {
  peer_tls::ConfigureSocket(fd);
  Require(::fcntl(fd, F_SETFL, O_NONBLOCK) == 0,
          "Cannot configure nonblocking execution socket");
}
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
    const auto address = unix_socket::Address(endpoint.substr(5));
    std::memcpy(&result.storage, &address, sizeof address);
    result.size = sizeof address;
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
// The hostd project id of a project folder: the one hostd reserved for that
// folder when starting its environment (.tsugumi/project-id; the registry
// refuses an id registered to another folder). "" when there is none.
auto RegisteredProject(const std::filesystem::path& root) -> std::string {
  const auto id = fs::read_file(root / ".tsugumi" / "project-id");
  return id && crypto::is_short_id(*id) ? *id : std::string{};
}
struct Entry {
  std::string id, name, endpoint, worker_public_key;
  R::CapabilitiesT capabilities;
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
  struct ProbeEntry {
    std::string project;
    std::vector<std::string> card;
    std::optional<Tsugumi::Data::WorkerProbeResultT> measurement;
    bool reusable = false;
  };
  struct ProbeCache {
    std::string life;
    std::string revision = crypto::random_token(32);
    std::map<std::string, ProbeEntry> results;
  };
  std::map<std::string, ProbeCache> probes;
  std::map<std::string, std::string> local_probe_owners;
  std::set<int> active;
  bool stopping = false;
  Socket listener, cancel_listener, wake_reader, wake_writer;
  unix_socket::Binding binding, cancel_binding;
  struct Activity {
    State& state;
    int fd;
    ~Activity() {
      std::lock_guard lock(state.mutex);
      state.active.erase(fd);
    }
  };
  struct Client {
    Client(State& state, Socket accepted)
        : socket(std::move(accepted)), activity(state.Track(socket.fd)) {}
    Socket socket;
    Activity activity;
  };
  struct Transfer {
    std::unique_ptr<Client> client;
    std::unique_ptr<R::MessageT> message;
    Deadline deadline;
    size_t bytes;
  };
  std::deque<Transfer> pending;
  size_t pending_bytes = 0;
  std::condition_variable transfer_ready;
  std::vector<std::jthread> receivers, transfers;
  CardSecrets card_secrets;
  MeasureProbe measure_probe;
  std::mutex remeasure_mutex;

  State(std::filesystem::path dir, std::string path, CardSecrets secrets,
        MeasureProbe measure)
      : state(std::move(dir)),
        socket(std::move(path)),
        db((state / "execution.sqlite").string(),
           sql::FormatVersion{kExecutionFormatVersion}),
        identity(state / "execution-identity.pem"),
        card_secrets(std::move(secrets)),
        measure_probe(std::move(measure)) {
    ::chmod((state / "execution.sqlite").c_str(), 0600);
    db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
    db.initialize_format(kExecutionFormatVersion, [&] {
      db.exec(
          "CREATE TABLE IF NOT EXISTS services(id TEXT PRIMARY KEY,name TEXT "
          "NOT "
          "NULL,"
          "endpoint TEXT NOT NULL,worker_public_key TEXT NOT NULL,capabilities "
          "BLOB NOT "
          "NULL,generation TEXT NOT NULL);"
          "CREATE TABLE IF NOT EXISTS projects(id TEXT PRIMARY KEY,identity "
          "TEXT "
          "NOT NULL,"
          "scope TEXT NOT NULL,capability TEXT NOT NULL UNIQUE);"
          "CREATE TABLE IF NOT EXISTS project_services(project TEXT NOT NULL,"
          "service TEXT NOT NULL,generation TEXT NOT NULL,PRIMARY "
          "KEY(project,service));"
          // The hostd project id each authorized core belongs to; Card secrets
          // are resolved only within it.
          "CREATE TABLE IF NOT EXISTS project_registry(project TEXT PRIMARY "
          "KEY,registry TEXT NOT NULL)");
    });
    const auto file = state / "execution-scope-key";
    key = ReadKey(file);
    if (key.empty()) {
      fs::write_atomic(file, crypto::random_token(32), 0600);
      key = ReadKey(file);
    }
    Listen(listener, binding, socket);
    Listen(cancel_listener, cancel_binding, socket + ".c");
    int wake[2];
    Require(::pipe(wake) == 0, "Cannot open execution broker wake pipe");
    wake_reader.fd = wake[0];
    wake_writer.fd = wake[1];
    Require(::fcntl(wake_reader.fd, F_SETFD, FD_CLOEXEC) == 0 &&
                ::fcntl(wake_writer.fd, F_SETFD, FD_CLOEXEC) == 0,
            "Cannot configure execution broker wake pipe");
    // Keep broker-local replies and Cancel independent of worker transfers.
    try {
      for (size_t i = 0; i < kTransferSlots; ++i)
        transfers.emplace_back([this] { RunTransfers(); });
      for (int i = 0; i < 2; ++i) {
        receivers.emplace_back([this] { Accept(listener.fd, false); });
        receivers.emplace_back([this] { Accept(cancel_listener.fd, true); });
      }
    } catch (...) {
      Stop();
      throw;
    }
  }

  static auto Listen(Socket& listener, unix_socket::Binding& binding,
                     const std::string& path) -> void {
    listener.fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    Require(listener.fd >= 0, "Cannot open execution broker listener");
    Configure(listener.fd);
    binding.Bind(listener.fd, path);
    Require(::listen(listener.fd, kPendingTransfers) == 0,
            "Cannot listen on execution broker socket");
  }

  auto Accept(int source, bool cancel_only) -> void {
    for (;;) {
      pollfd ready[]{{source, POLLIN, 0}, {wake_reader.fd, POLLIN, 0}};
      if (::poll(ready, 2, -1) < 0) {
        if (errno == EINTR) continue;
        log::error("Execution broker listener poll failed");
        return;
      }
      if (ready[1].revents) return;
      Socket client{::accept(source, nullptr, nullptr)};
      if (client.fd < 0) {
        if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN ||
            errno == EWOULDBLOCK)
          continue;
        log::error("Execution broker accept failed: " +
                   std::string(std::strerror(errno)));
        return;
      }
      try {
        Configure(client.fd);
        Dispatch(std::make_unique<Client>(*this, std::move(client)),
                 cancel_only);
      } catch (const std::exception&) {
        // An incomplete frame or disconnected caller has no reply channel.
      }
    }
  }

  auto Dispatch(std::unique_ptr<Client> client, bool cancel_only) -> void {
    const auto deadline = Clock::now() + kExchangeTimeout;
    const int fd = client->socket.fd;
    // Decode before dispatch so catalog, secrets and probe cache requests
    // can be answered while every worker transfer is in flight.
    const auto bytes = Read(fd, deadline);
    auto message = W::decode(bytes);
    const auto* broker = message->payload.AsBrokerRequest();
    if (broker && !cancel_only) {
      const auto& body = broker->request->body;
      if (!body.AsCatalogRequest() && !body.AsCardSecretsRequest() &&
          !body.AsCardProbeLookup() && !body.AsCardProbePublish() &&
          !body.AsCancelRequest()) {
        std::lock_guard lock(mutex);
        // EOF leaves a saturated request unconfirmed, like a transport loss.
        if (stopping || pending.size() >= kPendingTransfers ||
            bytes.size() > kPendingBytes - pending_bytes)
          return;
        pending.emplace_back();
        pending.back() = {std::move(client), std::move(message), deadline,
                          bytes.size()};
        pending_bytes += bytes.size();
        transfer_ready.notify_one();
        return;
      }
    }
    Forward(fd, cancel_only, *message, deadline);
  }

  auto RunTransfers() -> void {
    for (;;) {
      std::unique_lock lock(mutex);
      transfer_ready.wait(lock, [&] { return stopping || !pending.empty(); });
      if (stopping) return;
      auto transfer = std::move(pending.front());
      pending.pop_front();
      pending_bytes -= transfer.bytes;
      lock.unlock();
      try {
        Forward(transfer.client->socket.fd, false, *transfer.message,
                transfer.deadline);
      } catch (const std::exception&) {
        // The downstream can close while its request is queued.
      }
    }
  }

  auto Track(int fd) -> Activity {
    std::lock_guard lock(mutex);
    Require(!stopping, "Execution broker is stopping");
    active.insert(fd);
    return {*this, fd};
  }

  auto DecodeEntry(sql::Stmt& q) -> Entry {
    const auto bytes = q.blob(4);
    Require(bytes.size() <= W::kMaxFrame, "Invalid stored worker capabilities");
    auto message = W::decode(bytes);
    const auto* reply = message->payload.AsReply();
    Require(reply && reply->body.AsCapabilities(),
            "Invalid stored worker capabilities");
    return {q.text(0),
            q.text(1),
            q.text(2),
            q.text(3),
            *reply->body.AsCapabilities(),
            q.text(5)};
  }

  auto LocalEntry() -> Entry {
    Entry local;
    local.id = "local";
    local.name = "local";
    local.capabilities.native_system = "local";
    local.capabilities.systems = {"local"};
    return local;
  }

  auto FindEntry(const std::string& id) -> std::optional<Entry> {
    auto q = db.prepare(
        "SELECT id,name,endpoint,worker_public_key,capabilities,"
        "generation FROM services WHERE id=?");
    q.bind(1, id);
    if (q.step()) return DecodeEntry(q);
    if (id == "local") return LocalEntry();
    return std::nullopt;
  }

  auto Entries() -> std::vector<Entry> {
    auto q = db.prepare(
        "SELECT id,name,endpoint,worker_public_key,capabilities,"
        "generation FROM services "
        "ORDER BY (id != 'local'),rowid");
    std::vector<Entry> entries;
    while (q.step()) entries.push_back(DecodeEntry(q));
    if (entries.empty() || entries.front().id != "local")
      entries.insert(entries.begin(), LocalEntry());
    return entries;
  }
  auto List() -> H::ExecutionServicesT {
    H::ExecutionServicesT result;
    result.host_public_key = identity.public_key();
    for (const auto& entry : Entries()) {
      auto service = Public(entry);
      if (auto cache = probes.find(entry.id); cache != probes.end()) {
        for (const auto& [_, probe] : cache->second.results) {
          if (!probe.measurement) continue;
          // Settings summarize the results; bounded process output lives in
          // the invocation record that used it.
          auto summary = std::make_unique<Tsugumi::Data::WorkerProbeResultT>(
              *probe.measurement);
          summary->stdout_text.clear();
          summary->stderr_text.clear();
          service->probes.push_back(std::move(summary));
        }
        std::ranges::sort(
            service->probes, std::greater{},
            [](const auto& result) { return result->measured_at; });
      }
      result.services.push_back(std::move(service));
    }
    return result;
  }
  auto Remeasure(const std::string& worker) -> H::ExecutionServicesT {
    std::lock_guard remeasure(remeasure_mutex);
    struct Work {
      std::string worker, key, revision;
      ProbeEntry probe;
    };
    std::vector<Work> work;
    {
      std::lock_guard lock(mutex);
      if (!worker.empty())
        Require(FindEntry(worker).has_value(), "Unknown execution service");
      for (auto& [id, cache] : probes) {
        if (!worker.empty() && id != worker) continue;
        cache.revision = crypto::random_token(32);
        for (auto& [key, probe] : cache.results) {
          if (!probe.card.empty() && measure_probe)
            work.push_back({id, key, cache.revision, probe});
          probe.measurement.reset();
          probe.reusable = false;
        }
      }
    }
    for (const auto& item : work) {
      Tsugumi::Data::WorkerProbeResultT result;
      try {
        result = measure_probe(item.probe.project, item.probe.card, item.worker,
                               item.key);
      } catch (const std::exception& error) {
        result.worker = item.worker;
        if (item.probe.measurement)
          result.probe = item.probe.measurement->probe;
        result.failure = std::string(error.what()).substr(0, kProbeOutputBytes);
        result.measured_at =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
      }
      std::lock_guard lock(mutex);
      const auto cache = probes.find(item.worker);
      if (cache == probes.end() || cache->second.revision != item.revision)
        continue;
      const auto entry = cache->second.results.find(item.key);
      if (entry != cache->second.results.end() && !entry->second.reusable)
        entry->second.measurement = std::move(result);
    }
    std::lock_guard lock(mutex);
    return List();
  }

  auto Remote(const Entry& entry, R::RequestT request, Deadline deadline,
              int client = -1) -> R::MessageT {
    (void)Remaining(deadline);
    if (client >= 0) {
      pollfd downstream{client, POLLIN, 0};
      int ready;
      do {
        ready = ::poll(&downstream, 1, 0);
      } while (ready < 0 && errno == EINTR);
      Require(ready == 0, "Execution stream disconnected");
    }
    const auto address = ParseEndpoint(entry.endpoint);
    (void)Remaining(deadline);
    Socket peer{::socket(address.storage.ss_family, SOCK_STREAM, 0)};
    Require(peer.fd >= 0, "Cannot open worker connection");
    Configure(peer.fd);
    auto activity = Track(peer.fd);
    auto connected =
        ::connect(peer.fd, reinterpret_cast<const sockaddr*>(&address.storage),
                  address.size) == 0;
    if (!connected && errno == EINPROGRESS) {
      WaitForSocket(peer.fd, POLLOUT, deadline, client);
      int error = 0;
      socklen_t size = sizeof error;
      connected =
          ::getsockopt(peer.fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
          !error;
    }
    Require(connected, "Cannot connect to remote worker");
    peer_tls::Context context(identity, false, [&](std::string_view key) {
      return key == entry.worker_public_key;
    });
    peer_tls::Stream tls(context, peer.fd, false, client, deadline);
    const auto* poll = request.body.AsStatusRequest();
    const bool view_message =
        request.body.AsViewSendRequest() ||
        (poll && (poll->view_reply || poll->display_after ||
                  !poll->display_credits.empty()));
    R::MessageT message;
    message.payload.Set(std::move(request));
    auto bytes = W::encode(std::move(message));
    tls.Write(W::length_prefix(static_cast<uint32_t>(bytes.size())) + bytes);
    trace::Link link;
    if (!view_message)
      trace::Observe(link, "hostd execution broker", "remote worker service",
                     true, bytes, &trace::kFlatBufferCodec);
    const auto length = W::frame_length(tls.Read(4));
    const auto frame = tls.Read(length);
    auto reply = W::decode(frame);
    // Status can carry transient view traffic even in reply to a plain poll.
    const auto* answer = reply->payload.AsReply();
    const auto* status = answer ? answer->body.AsJobStatus() : nullptr;
    if (!view_message &&
        (!status ||
         (!status->view_request && !status->display_pending &&
          status->displays.empty() && status->view_input_credits.empty())))
      trace::Observe(link, "remote worker service", "hostd execution broker",
                     false, frame, &trace::kFlatBufferCodec);
    Require(reply->payload.AsReply(), "Expected remote worker reply");
    return std::move(*reply);
  }
  auto Forward(int fd, bool cancel_only, R::MessageT& message,
               Deadline deadline) -> void {
    bool forwarding = false;
    try {
      (void)Remaining(deadline);
      auto* broker = message.payload.AsBrokerRequest();
      Require(broker, "Expected a private broker envelope");
      auto* request = broker->request.get();
      Require(cancel_only == (request->body.AsCancelRequest() != nullptr),
              "Wrong execution broker endpoint for this request");
      Entry entry;
      std::optional<R::CatalogT> catalog_reply;
      std::optional<R::MessageT> probe_reply;
      std::optional<std::vector<std::string>> secret_names;
      std::string secret_project;
      {
        std::lock_guard lock(mutex);
        auto q = db.prepare("SELECT scope,id FROM projects WHERE capability=?");
        q.bind(1, broker->capability);
        if (!q.step())
          throw W::Error(R::ErrorCode::Unauthorized,
                         "Project is not authorized to use this broker");
        request->project_scope = q.text(0);
        if (auto* asked = request->body.AsCardSecretsRequest()) {
          Require(card_secrets != nullptr, "Card secrets are unavailable");
          Require(asked->names.size() <= 256, "Too many Card secret names");
          // Only the project this capability was issued to, never one the
          // request names.
          auto registered = db.prepare(
              "SELECT registry FROM project_registry WHERE project=?");
          registered.bind(1, q.text(1));
          Require(registered.step() && !registered.text(0).empty(),
                  "This project has no hostd project id for Card secrets");
          secret_project = registered.text(0);
          secret_names = std::move(asked->names);
        } else if (request->body.AsCatalogRequest()) {
          R::CatalogT catalog;
          for (const auto& e : Entries()) {
            auto target = std::make_unique<R::TargetT>();
            target->id = e.id;
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
            if (const auto cache = probes.find(e.id); cache != probes.end())
              target->probe_revision = cache->second.revision;
            catalog.services.push_back(std::move(target));
          }
          catalog_reply = std::move(catalog);
        } else {
          const auto* lookup = request->body.AsCardProbeLookup();
          const auto* publish = request->body.AsCardProbePublish();
          auto found = FindEntry(request->service);
          if (!found ||
              (found->id == "local" ? !lookup && !publish
                                    : found->capabilities.server_id.empty()))
            throw W::Error(
                R::ErrorCode::RegistrationChanged,
                "Verify the execution service registration in settings");
          auto grant = db.prepare(
              "SELECT generation FROM project_services "
              "WHERE project=? AND service=?");
          grant.bind(1, q.text(1)).bind(2, found->id);
          if (found->id != "local" &&
              (!grant.step() || grant.text(0) != found->generation))
            throw W::Error(R::ErrorCode::RegistrationChanged,
                           "Execution service registration changed; "
                           "reauthorize this project");
          entry = std::move(*found);
          if (lookup || publish) {
            const auto& key = lookup ? lookup->key : publish->key;
            const auto& life = lookup ? lookup->life : publish->life;
            Require(crypto::is_sha256_hex(key) && crypto::is_sha256_hex(life),
                    "Invalid worker probe cache identity");
            if (entry.id != "local")
              Require(life == entry.capabilities.probe_life,
                      "Worker restarted before probe cache access");
            if (lookup && entry.id == "local") {
              Require(crypto::is_sha256_hex(request->core_life_id),
                      "Missing local worker owner identity");
              const auto [owner, added] = local_probe_owners.try_emplace(
                  q.text(1), request->core_life_id);
              if (!added && owner->second != request->core_life_id) {
                probes.erase(entry.id);
                owner->second = request->core_life_id;
              }
            }
            auto& cache = probes[entry.id];
            if (cache.life != life) {
              cache = ProbeCache{};
              cache.life = life;
            }
            if (lookup) {
              R::CardProbeCachedT answer;
              answer.revision = cache.revision;
              if (auto item = cache.results.find(key);
                  item != cache.results.end() && item->second.reusable)
                answer.result =
                    std::make_unique<Tsugumi::Data::WorkerProbeResultT>(
                        *item->second.measurement);
              probe_reply = W::reply(std::move(answer));
            } else {
              const auto& measured = *publish->result;
              Require(measured.worker == entry.id &&
                          measured.probe.size() <= 512 &&
                          measured.failure.size() <= kProbeOutputBytes &&
                          measured.stdout_text.size() +
                                  measured.stderr_text.size() <=
                              kProbeOutputBytes &&
                          (measured.score.has_value() !=
                           !measured.failure.empty()) &&
                          (!measured.score || std::isfinite(*measured.score)),
                      "Invalid worker probe result");
              if (cache.revision == publish->revision) {
                constexpr size_t kResultsPerWorker = 128;
                if (cache.results.size() >= kResultsPerWorker &&
                    !cache.results.contains(key)) {
                  const auto oldest = std::ranges::min_element(
                      cache.results, {}, [](const auto& item) {
                        return item.second.measurement
                                   ? item.second.measurement->measured_at
                                   : 0;
                      });
                  cache.results.erase(oldest);
                }
                Require(publish->card.size() <= 64 &&
                            std::ranges::all_of(publish->card,
                                                [](const auto& part) {
                                                  return !part.empty() &&
                                                         part.size() <= 64;
                                                }),
                        "Invalid probe Card path");
                auto registry = db.prepare(
                    "SELECT registry FROM project_registry WHERE project=?");
                registry.bind(1, q.text(1));
                auto project =
                    registry.step() ? registry.text(0) : std::string{};
                if (project.empty()) project = q.text(1);
                auto& probe = cache.results[key];
                if (!probe.reusable)
                  probe = {std::move(project), publish->card, measured, true};
              }
              probe_reply = W::reply(R::AckT{});
            }
          }
        }
      }
      if (probe_reply) {
        Write(fd, std::move(*probe_reply), deadline);
        return;
      }
      if (catalog_reply) {
        Write(fd, W::reply(std::move(*catalog_reply)), deadline);
        return;
      }
      // Read outside the broker lock: the store has its own.
      if (secret_names) {
        R::CardSecretsT secrets;
        for (auto& [name, value] :
             card_secrets(secret_project, *secret_names)) {
          auto secret = std::make_unique<R::CardSecretT>();
          secret->name = std::move(name);
          secret->value = std::move(value);
          secrets.secrets.push_back(std::move(secret));
        }
        Write(fd, W::reply(std::move(secrets)), deadline);
        return;
      }
      forwarding = true;
      const bool probing = request->body.AsProbeRequest() != nullptr;
      auto response = Remote(entry, std::move(*request), deadline, fd);
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
          if (const auto cache = probes.find(entry.id);
              cache != probes.end() && cache->second.life != cached.probe_life)
            probes.erase(cache);
          auto update = db.prepare(
              "UPDATE services SET capabilities=? WHERE id=? AND generation=?");
          update.bind_blob(1, W::encode(W::reply(std::move(cached))))
              .bind(2, entry.id)
              .bind(3, entry.generation)
              .run();
        }
      }
      Write(fd, std::move(response), deadline);
    } catch (const std::exception& e) {
      // A transport interruption after forwarding leaves submit unconfirmed.
      const auto* worker_error = dynamic_cast<const W::Error*>(&e);
      if (forwarding && !worker_error) return;
      Write(
          fd,
          W::failure(worker_error ? worker_error->code : R::ErrorCode::Invalid,
                     e.what()),
          deadline);
    }
  }
  auto Stop() -> void {
    {
      std::lock_guard lock(mutex);
      if (stopping) return;
      stopping = true;
      for (int fd : active) ::shutdown(fd, SHUT_RDWR);
      while (::write(wake_writer.fd, "x", 1) < 0 && errno == EINTR) {
      }
    }
    transfer_ready.notify_all();
    receivers.clear();
    transfers.clear();
    pending.clear();
    binding.Remove();
    cancel_binding.Remove();
  }
};

ExecutionBroker::ExecutionBroker(std::filesystem::path state,
                                 std::string socket, CardSecrets card_secrets,
                                 MeasureProbe measure_probe)
    : state_(std::make_unique<State>(std::move(state), std::move(socket),
                                     std::move(card_secrets),
                                     std::move(measure_probe))) {}
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
    state_->db.prepare("DELETE FROM project_registry WHERE project=?")
        .bind(1, project)
        .run();
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
  state_->db
      .prepare(
          "INSERT INTO project_services SELECT ?,id,generation FROM services")
      .bind(1, project)
      .run();
  state_->db.prepare("INSERT OR REPLACE INTO project_registry VALUES(?,?)")
      .bind(1, project)
      .bind(2, RegisteredProject(path))
      .run();
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
  if (state_->local_probe_owners.erase(project)) state_->probes.erase("local");
  auto q = state_->db.prepare("DELETE FROM projects WHERE id=?");
  q.bind(1, project).run();
  state_->db.prepare("DELETE FROM project_registry WHERE project=?")
      .bind(1, project)
      .run();
  state_->db.prepare("DELETE FROM project_services WHERE project=?")
      .bind(1, project)
      .run();
}
auto ExecutionBroker::Handles(H::RequestPayload type) -> bool {
  return type == H::RequestPayload::ExecutionServicesRequest ||
         type == H::RequestPayload::ExecutionServicePutRequest ||
         type == H::RequestPayload::ExecutionProbeRemeasureRequest ||
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
        if (const auto e = state_->FindEntry(entry.id)) {
          previous =
              e->generation + e->endpoint + e->worker_public_key + e->name;
          if (entry.endpoint == e->endpoint &&
              entry.worker_public_key == e->worker_public_key)
            entry.capabilities = e->capabilities;
          entry.generation = e->generation;
        }
      }
      if (entry.id != "local") peer_tls::ValidateKey(entry.worker_public_key);
      if (entry.id != "local" && p->verify()) {
        R::RequestT probe;
        probe.body.Set(R::ProbeRequestT{});
        auto response = state_->Remote(entry, std::move(probe),
                                       Clock::now() + kExchangeTimeout);
        auto* reply = response.payload.AsReply();
        if (reply->body.AsFailure())
          throw std::runtime_error(
              "The remote worker rejected the registration");
        entry.capabilities = VerifiedCapabilities(*reply);
        entry.capabilities.instance_id.clear();
      }
      std::lock_guard lock(state_->mutex);
      std::string current;
      if (const auto e = state_->FindEntry(entry.id)) {
        current = e->generation + e->endpoint + e->worker_public_key + e->name;
        if (e->endpoint != entry.endpoint ||
            e->worker_public_key != entry.worker_public_key ||
            e->capabilities.server_id != entry.capabilities.server_id)
          entry.generation = crypto::random_token(32);
        else if (!p->verify())
          entry.capabilities = e->capabilities;
      }
      const auto caps = W::encode(W::reply(entry.capabilities));
      Require(current == previous,
              "Registration changed while verifying; reopen settings");
      sql::Transaction transaction(state_->db);
      auto q = state_->db.prepare(
          "INSERT INTO services VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO "
          "UPDATE SET "
          "name=excluded.name,endpoint=excluded.endpoint,worker_public_key="
          "excluded.worker_public_key,capabilities=excluded.capabilities,"
          "generation=excluded.generation");
      q.bind(1, entry.id)
          .bind(2, entry.name)
          .bind(3, entry.endpoint)
          .bind(4, entry.worker_public_key)
          .bind_blob(5, caps)
          .bind(6, entry.generation)
          .run();
      transaction.commit();
      state_->probes.erase(entry.id);
      return agent::wire::HostReply(id, state_->List());
    }
    if (const auto* p = request.value_as_ExecutionProbeRemeasureRequest()) {
      Require(!p->id() || p->id()->size() > 0, "Invalid execution service ID");
      return agent::wire::HostReply(
          id, state_->Remeasure(p->id() ? p->id()->str() : ""));
    }
    std::lock_guard lock(state_->mutex);
    if (const auto* p = request.value_as_ExecutionServiceRemoveRequest()) {
      Require(ValidId(p->id()->str()), "Invalid execution service ID");
      sql::Transaction transaction(state_->db);
      auto q = state_->db.prepare("DELETE FROM services WHERE id=?");
      q.bind(1, p->id()->str()).run();
      transaction.commit();
      state_->probes.erase(p->id()->str());
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
