// Public Host management RPC and independent remote-worker wire peer.
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <utility>

#include "fixture.hh"
#include "tsugumi/rpc/peer_tls.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/sqlite.hh"
#include "tsugumi/workerd/protocol.hh"

namespace tsugumi::test {
namespace {
namespace R = Tsugumi::RemoteWorker;
namespace W = tsugumi::workerd;
struct Socket {
  int fd = -1;
  ~Socket() {
    if (fd >= 0) ::close(fd);
  }
};
auto Transfer(int fd, std::string& data, bool send) -> void {
  size_t offset = 0;
  while (offset < data.size()) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto n =
        send ? ::send(fd, data.data() + offset, data.size() - offset, flags)
             : ::read(fd, data.data() + offset, data.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("fixture stream disconnected");
    offset += static_cast<size_t>(n);
  }
}
auto ReadMessage(int fd) -> std::unique_ptr<R::MessageT> {
  std::string prefix(4, '\0');
  Transfer(fd, prefix, false);
  std::string bytes(W::frame_length(prefix), '\0');
  Transfer(fd, bytes, false);
  return W::decode(bytes);
}
auto SendMessage(int fd, R::MessageT message) -> void {
  auto bytes = W::encode(std::move(message));
  auto prefix = W::length_prefix(static_cast<uint32_t>(bytes.size()));
  Transfer(fd, prefix, true);
  Transfer(fd, bytes, true);
}
auto ConnectUnix(const std::string& path) -> int {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof address.sun_path)
    throw std::runtime_error("fixture path too long");
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  Socket socket{::socket(AF_UNIX, SOCK_STREAM, 0)};
  if (::connect(socket.fd, reinterpret_cast<sockaddr*>(&address),
                sizeof address) != 0)
    throw std::runtime_error("fixture connect failed: " + path);
  peer_tls::ConfigureSocket(socket.fd, 3);
  const auto fd = socket.fd;
  socket.fd = -1;
  return fd;
}
class WorkerPeer {
 public:
  explicit WorkerPeer(const std::filesystem::path& path)
      : identity(path.parent_path() / "worker-identity.pem"),
        path_(path.string()),
        authorized_(path.parent_path() / "authorized_keys"),
        tls_(identity, true, [this](std::string_view key) {
          return peer_tls::Authorized(authorized_, key);
        }) {
    socket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
    if (::bind(socket_, reinterpret_cast<sockaddr*>(&address),
               sizeof address) ||
        ::listen(socket_, 16))
      throw std::runtime_error("fixture listen failed");
    thread_ = std::jthread([this] {
      while (true) {
        Socket peer{::accept(socket_, nullptr, nullptr)};
        if (peer.fd < 0) return;
        if (stopping_) return;
        try {
          peer_tls::ConfigureSocket(peer.fd, 3);
          peer_tls::Stream tls(tls_, peer.fd, true);
          const auto size = W::frame_length(tls.Read(4));
          auto message = W::decode(tls.Read(size));
          const auto respond = [&](R::MessageT value) {
            auto bytes = W::encode(std::move(value));
            tls.Write(W::length_prefix(static_cast<uint32_t>(bytes.size())) +
                      bytes);
          };
          auto* q = message->payload.AsRequest();
          if (!q) continue;
          std::lock_guard lock(mutex_);
          requests_.push_back(*q);
          if (q->body.AsProbeRequest()) {
            R::CapabilitiesT c;
            c.server_id = server;
            c.instance_id = std::string(64, 'c');
            c.native_system = "x86_64-linux";
            c.systems = {c.native_system};
            c.max_jobs = 4;
            c.resource_limits = resource_limits;
            c.worker_contract = contract;
            c.revision = revision;
            c.probe_life = probe_life;
            respond(W::reply(std::move(c)));
          } else if (auto* put = q->body.AsPutBlobRequest()) {
            R::BlobChunkT chunk;
            chunk.digest = put->digest;
            chunk.total = put->total;
            chunk.offset = put->offset;
            chunk.bytes = put->bytes;
            respond(W::reply(std::move(chunk)));
          } else if (q->body.AsStatusRequest()) {
            // A held reply keeps the broker forwarding while this fixture
            // continues to accept new connections.
            if (hold_status_) {
              held_status_.push_back(peer.fd);
              peer.fd = -1;
              status_seen_.notify_all();
            }
          } else
            respond(W::reply(R::AckT{}));
        } catch (...) {
        }
      }
    });
  }
  ~WorkerPeer() {
    stopping_ = true;
    // Wake accept portably on macOS as well as Linux.
    try {
      Socket wake{ConnectUnix(path_)};
    } catch (...) {
    }
    thread_.join();
    for (int fd : held_status_) ::close(fd);
    ::close(socket_);
    ::unlink(path_.c_str());
  }
  auto HoldStatus() -> void {
    std::lock_guard lock(mutex_);
    hold_status_ = true;
  }
  auto WaitForHeldStatus(size_t count) -> bool {
    std::unique_lock lock(mutex_);
    return status_seen_.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return held_status_.size() == count; });
  }
  auto Requests() -> std::vector<R::RequestT> {
    std::lock_guard lock(mutex_);
    return requests_;
  }
  peer_tls::Identity identity;
  auto Authorize(std::string_view key) -> void {
    Write(authorized_, std::string(key) + "\n");
  }
  auto SetServer(std::string value) -> void {
    std::lock_guard lock(mutex_);
    server = std::move(value);
  }
  // A worker built from another revision speaks another contract.
  auto SetBuild(uint32_t value, std::string from) -> void {
    std::lock_guard lock(mutex_);
    contract = value;
    revision = std::move(from);
  }
  auto SetResourceLimits(bool value) -> void {
    std::lock_guard lock(mutex_);
    resource_limits = value;
  }
  auto RestartProbeLife() -> void {
    std::lock_guard lock(mutex_);
    probe_life = crypto::random_token(32);
  }
  std::string probe_life = std::string(64, 'd');
  bool resource_limits = true;
  std::string server = std::string(64, 'b');
  uint32_t contract = W::kContract;
  std::string revision = "0123abcd";

 private:
  std::string path_;
  std::filesystem::path authorized_;
  peer_tls::Context tls_;
  int socket_;
  std::atomic<bool> stopping_ = false;
  std::mutex mutex_;
  std::vector<R::RequestT> requests_;
  std::condition_variable status_seen_;
  bool hold_status_ = false;
  std::vector<int> held_status_;
  std::jthread thread_;
};
class BrokerFixture : public AgentFixture {
 protected:
  auto SetUp() -> void override {
    // Like both production entry points, suppress SIGPIPE for OpenSSL's BIO
    // writes as well as using per-send/per-socket protection for plain frames.
    ::signal(SIGPIPE, SIG_IGN);
    AgentFixture::SetUp();
    worker = std::make_unique<WorkerPeer>(directory.path / "worker.sock");
    const auto reply = List();
    worker->Authorize(reply.HostValue()
                          ->value_as_ExecutionServices()
                          ->host_public_key()
                          ->str());
  }
  auto Put(std::string key = {}, std::string id = "worker",
           std::string endpoint = {}, bool verify = true,
           std::string name = "Test worker") -> Reply {
    return Call(
        *host, H::RequestPayload::ExecutionServicePutRequest, [&](Builder& b) {
          return H::CreateExecutionServicePutRequest(
                     b, b.CreateString(id), b.CreateString(name),
                     b.CreateString(
                         id != "local" && endpoint.empty()
                             ? "unix:" +
                                   (directory.path / "worker.sock").string()
                             : endpoint),
                     b.CreateString(id != "local" && key.empty()
                                        ? worker->identity.public_key()
                                        : key),
                     verify)
              .Union();
        });
  }
  auto List() -> Reply {
    return Call(*host, H::RequestPayload::ExecutionServicesRequest,
                [](Builder& b) {
                  return H::CreateExecutionServicesRequest(b).Union();
                });
  }
  auto Authorize(const std::string& project,
                 const std::string& identity = "original") -> std::string {
    auto control = directory.path / project;
    std::filesystem::create_directories(control);
    auto folder = directory.path / ("project-" + identity);
    std::filesystem::create_directories(folder);
    auto response =
        Call(*host, H::RequestPayload::ExecutionProjectAuthorizeRequest,
             [&](Builder& b) {
               return H::CreateExecutionProjectAuthorizeRequest(
                          b, b.CreateString(project),
                          b.CreateString(folder.string()),
                          b.CreateString(control.string()))
                   .Union();
             });
    if (!response.Code().empty()) throw std::runtime_error(response.Code());
    EXPECT_EQ(response.HostValue()
                  ->value_as_ExecutionAuthorization()
                  ->endpoint()
                  ->str(),
              JsonString(ready, "/execution_broker"));
    const auto capability = Read(control / "execution-broker-capability");
    EXPECT_EQ(std::string(response.bytes.begin(), response.bytes.end())
                  .find(capability),
              std::string::npos);
    struct stat st{};
    ::stat((control / "execution-broker-capability").c_str(), &st);
    EXPECT_EQ(st.st_mode & 0077, 0);
    return capability;
  }
  auto Revoke(const std::string& project) -> void {
    EXPECT_TRUE(Call(*host, H::RequestPayload::ExecutionProjectRevokeRequest,
                     [&](Builder& b) {
                       return H::CreateExecutionProjectRevokeRequest(
                                  b, b.CreateString(project))
                           .Union();
                     })
                    .Code()
                    .empty());
  }
  template <class T>
  auto SendBroker(T body, const std::string& capability) -> int {
    auto endpoint = JsonString(ready, "/execution_broker").substr(5);
    if constexpr (std::is_same_v<T, R::CancelRequestT>) endpoint += ".c";
    Socket socket{ConnectUnix(endpoint)};
    R::RequestT request;
    request.service = "worker";
    request.project_scope = "SPOOFED";
    request.instance_id = "instance";
    request.job_id = "job";
    request.body.Set(std::move(body));
    R::MessageT message;
    R::BrokerRequestT broker;
    broker.capability = capability;
    broker.request = std::make_unique<R::RequestT>(std::move(request));
    message.payload.Set(std::move(broker));
    SendMessage(socket.fd, std::move(message));
    return std::exchange(socket.fd, -1);
  }
  template <class T>
  auto Broker(T body, const std::string& capability)
      -> std::unique_ptr<R::MessageT> {
    const Socket socket{SendBroker(std::move(body), capability)};
    return ReadMessage(socket.fd);
  }
  auto Restart() -> void {
    host.reset();
    human.reset();
    external.reset();
    service.Stop();
    StartHost();
  }
  std::unique_ptr<WorkerPeer> worker;
};
TEST_F(BrokerFixture, RegistrationProbesPersistsAndNeverReturnsCredentials) {
  EXPECT_EQ(
      ListSize(List().HostValue()->value_as_ExecutionServices()->services()),
      1u);
  auto response = Put();
  ASSERT_TRUE(response.Code().empty());
  const auto* values =
      response.HostValue()->value_as_ExecutionServices()->services();
  ASSERT_EQ(ListSize(values), 2u);
  EXPECT_EQ(values->Get(1)->capabilities()->server_id()->str(),
            std::string(64, 'b'));
  EXPECT_EQ(std::string(response.bytes.begin(), response.bytes.end())
                .find("PRIVATE KEY"),
            std::string::npos);
  auto requests = worker->Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_TRUE(requests[0].project_scope.empty());
  Restart();
  EXPECT_EQ(
      ListSize(List().HostValue()->value_as_ExecutionServices()->services()),
      2u);
  EXPECT_TRUE(Put("").Code().empty());
  EXPECT_FALSE(Put("not-a-key").Code().empty());
  EXPECT_EQ(
      ListSize(List().HostValue()->value_as_ExecutionServices()->services()),
      2u);
  const auto count = worker->Requests().size();
  EXPECT_FALSE(Put(worker->identity.public_key(), "local").Code().empty());
  EXPECT_FALSE(Put(worker->identity.public_key(), "bad ID").Code().empty());
  EXPECT_FALSE(
      Put(worker->identity.public_key(), "worker", "http://example.com:443")
          .Code()
          .empty());
  EXPECT_EQ(worker->Requests().size(), count);
  const auto removed = Call(
      *host, H::RequestPayload::ExecutionServiceRemoveRequest, [](Builder& b) {
        return H::CreateExecutionServiceRemoveRequest(b,
                                                      b.CreateString("worker"))
            .Union();
      });
  EXPECT_EQ(
      ListSize(removed.HostValue()->value_as_ExecutionServices()->services()),
      1u);
  Restart();
  EXPECT_EQ(
      ListSize(List().HostValue()->value_as_ExecutionServices()->services()),
      1u);
}
auto CatalogService(const R::CatalogT& catalog, std::string_view id)
    -> const R::TargetT& {
  const auto found = std::ranges::find_if(
      catalog.services, [&](const auto& service) { return service->id == id; });
  if (found == catalog.services.end())
    throw std::runtime_error("Missing execution service: " + std::string(id));
  return **found;
}

TEST_F(BrokerFixture, SharesProbeResultsAndInvalidatesMeasurements) {
  ASSERT_TRUE(Put().Code().empty());
  const auto project = Authorize("probe-a");
  const auto other = Authorize("probe-b", "different-project");
  const auto key = crypto::sha256_hex("one probe definition");
  auto life = std::string(64, 'd');
  const auto refresh_life = [&] {
    const auto live = Broker(R::ProbeRequestT{}, project);
    const auto* capabilities = live->payload.AsReply()->body.AsCapabilities();
    ASSERT_NE(capabilities, nullptr);
    life = capabilities->probe_life;
  };
  const auto lookup = [&](const std::string& scope) {
    R::CardProbeLookupT request;
    request.key = key;
    request.life = life;
    auto reply = Broker(std::move(request), scope);
    const auto* cached = reply->payload.AsReply()->body.AsCardProbeCached();
    if (!cached) throw std::runtime_error("Expected a Card probe cache reply");
    return *cached;
  };
  const auto publish = [&](const std::string& revision, double score) {
    R::CardProbePublishT request;
    request.key = key;
    request.life = life;
    request.revision = revision;
    request.result = std::make_unique<Tsugumi::Data::WorkerProbeResultT>();
    request.result->worker = "worker";
    request.result->probe = "/nix/store/probe";
    request.result->score = score;
    request.result->output_text = "benchmark output\n";
    request.result->measured_at = 42;
    auto reply = Broker(std::move(request), project);
    EXPECT_NE(reply->payload.AsReply()->body.AsAck(), nullptr);
  };
  refresh_life();
  const auto first = lookup(project);
  EXPECT_FALSE(first.result);
  publish(first.revision, 0);
  auto shared = lookup(other);
  ASSERT_TRUE(shared.result);
  EXPECT_EQ(shared.result->score.value(), 0);
  EXPECT_EQ(shared.result->output_text, "benchmark output\n");
  EXPECT_EQ(shared.result->measured_at, 42);

  worker->RestartProbeLife();
  refresh_life();
  ASSERT_FALSE(lookup(other).result);
  publish(lookup(project).revision, 2.5);
  ASSERT_TRUE(lookup(other).result);

  ASSERT_TRUE(Put({}, "worker", {}, false, "Renamed worker").Code().empty());
  EXPECT_FALSE(lookup(other).result);
  const auto revision = lookup(project).revision;
  publish(revision, 2.5);
  for (bool one : {true, false}) {
    const auto remeasure =
        Call(*host, H::RequestPayload::ExecutionProbeRemeasureRequest,
             [&](Builder& b) {
               return H::CreateExecutionProbeRemeasureRequest(
                          b, one ? b.CreateString("worker")
                                 : flatbuffers::Offset<flatbuffers::String>{})
                   .Union();
             });
    ASSERT_TRUE(remeasure.Code().empty());
    EXPECT_FALSE(lookup(other).result);
    publish(revision, 2.5);  // A response from before remeasurement is stale.
    EXPECT_FALSE(lookup(other).result);
    publish(lookup(project).revision, 2.5);
    EXPECT_TRUE(lookup(other).result);
  }
}

TEST_F(BrokerFixture, CatalogExplainsProjectApprovalUntilGranted) {
  const auto capability = Authorize("project");
  ASSERT_TRUE(Put().Code().empty());
  const auto catalog = Broker(R::CatalogRequestT{}, capability);
  const auto* services = catalog->payload.AsReply()->body.AsCatalog();
  ASSERT_NE(services, nullptr);
  const auto& worker = CatalogService(*services, "worker");
  EXPECT_FALSE(worker.available);
  EXPECT_FALSE(worker.unavailable_reason.empty());
  EXPECT_EQ(Authorize("project"), capability);
  const auto granted = Broker(R::CatalogRequestT{}, capability);
  const auto& approved =
      CatalogService(*granted->payload.AsReply()->body.AsCatalog(), "worker");
  EXPECT_TRUE(approved.available);
  EXPECT_TRUE(approved.unavailable_reason.empty());
}
TEST_F(BrokerFixture, AWorkerFromAnotherRevisionIsOutOfDateUntilRebuilt) {
  const auto capability = Authorize("project");
  const auto service = [](const Reply& reply) {
    const auto* services = reply.HostValue()->value_as_ExecutionServices();
    EXPECT_EQ(services->worker_contract(), W::kContract);
    return services->services()->Get(1);
  };
  const auto catalog = [&] {
    const auto reply = Broker(R::CatalogRequestT{}, capability);
    const auto& found = CatalogService(
        *reply->payload.AsReply()->body.AsCatalog(), "worker");
    return std::pair{found.available, found.unavailable_reason};
  };
  const auto sentence = "Worker worker is out of date (worker contract " +
                        std::to_string(W::kContract - 1) +
                        ", revision old0rev; this Tsugumi uses contract " +
                        std::to_string(W::kContract) +
                        "). Rebuild the worker from this Tsugumi revision";

  // Verification records the worker as it reports itself and says it is out
  // of date; the catalog keeps it from runs with the same sentence.
  worker->SetBuild(W::kContract - 1, "old0rev");
  auto verified = Put();
  ASSERT_TRUE(verified.Code().empty());
  EXPECT_TRUE(service(verified)->outdated());
  EXPECT_EQ(service(verified)->capabilities()->worker_contract(),
            W::kContract - 1);
  EXPECT_EQ(service(verified)->capabilities()->revision()->str(), "old0rev");
  EXPECT_TRUE(service(List())->outdated());
  Authorize("project");
  const auto [available, reason] = catalog();
  EXPECT_FALSE(available);
  EXPECT_TRUE(reason.starts_with(sentence)) << reason;
  EXPECT_NE(reason.find("Update a worker"), std::string::npos);

  // Rebuilt and verified again, it is usable.
  worker->SetBuild(W::kContract, "new0rev");
  ASSERT_TRUE(Put().Code().empty());
  EXPECT_FALSE(service(List())->outdated());
  Authorize("project");
  EXPECT_TRUE(catalog().first);

  // A probe that finds it replaced by an old build is refused with the
  // sentence and records it, so settings and the next catalog say so too.
  worker->SetBuild(W::kContract - 1, "old0rev");
  const auto probe = Broker(R::ProbeRequestT{}, capability);
  const auto* failure = probe->payload.AsReply()->body.AsFailure();
  ASSERT_NE(failure, nullptr);
  EXPECT_EQ(failure->code, R::ErrorCode::Rejected);
  EXPECT_TRUE(failure->message.starts_with(sentence)) << failure->message;
  EXPECT_TRUE(service(List())->outdated());
  EXPECT_FALSE(catalog().first);
}
TEST_F(BrokerFixture, RelayOverwritesScopeAndPreservesChunkBytes) {
  ASSERT_TRUE(Put().Code().empty());
  auto a = Authorize("a"), b = Authorize("b", "different-directory");
  EXPECT_NE(a, b);
  auto catalog = Broker(R::CatalogRequestT{}, a);
  ASSERT_NE(catalog->payload.AsReply(), nullptr);
  const auto* catalog_reply = catalog->payload.AsReply();
  ASSERT_EQ(catalog_reply->body.type, R::ReplyBody::Catalog)
      << (catalog_reply->body.AsFailure()
              ? catalog_reply->body.AsFailure()->message
              : "unexpected worker reply");
  ASSERT_NE(catalog->payload.AsReply()->body.AsCatalog(), nullptr);
  EXPECT_TRUE(
      CatalogService(*catalog_reply->body.AsCatalog(), "worker").available);
  R::PutBlobRequestT put;
  put.digest = std::string(64, 'c');
  put.total = 192 * 1024;
  put.offset = 17;
  put.bytes = std::vector<uint8_t>(192 * 1024, 0xd3);
  auto first = Broker(put, a);
  ASSERT_NE(first->payload.AsReply()->body.AsBlobChunk(), nullptr);
  EXPECT_EQ(first->payload.AsReply()->body.AsBlobChunk()->bytes, put.bytes);
  auto scope = worker->Requests().back().project_scope;
  EXPECT_NE(scope, "SPOOFED");
  EXPECT_FALSE(scope.empty());
  Broker(put, b);
  EXPECT_NE(worker->Requests().back().project_scope, scope);
  // Same filesystem identity under a host alias shares the isolation scope.
  Broker(put, Authorize("alias"));
  EXPECT_EQ(worker->Requests().back().project_scope, scope);
  EXPECT_THROW(Broker(R::StatusRequestT{}, a), std::runtime_error);
}
TEST_F(BrokerFixture, RelayAndProbeDoNotDecodeUnrelatedRegistrations) {
  ASSERT_TRUE(Put().Code().empty());
  const auto capability = Authorize("project");
  {
    sql::Db db((directory.path / "state/execution.sqlite").string());
    db.exec(
        "INSERT INTO services VALUES "
        "('unrelated','Unrelated','','',X'00','')");
  }

  auto probe = Broker(R::ProbeRequestT{}, capability);
  ASSERT_NE(probe->payload.AsReply()->body.AsCapabilities(), nullptr);
  R::CardProbeLookupT lookup;
  lookup.key = std::string(64, 'a');
  lookup.life = worker->probe_life;
  auto cached = Broker(std::move(lookup), capability);
  EXPECT_NE(cached->payload.AsReply()->body.AsCardProbeCached(), nullptr);
  auto catalog = Broker(R::CatalogRequestT{}, capability);
  EXPECT_NE(catalog->payload.AsReply()->body.AsFailure(), nullptr);
}
TEST_F(BrokerFixture, AuthorizationSurvivesRestartAndRevocationIsDurable) {
  ASSERT_TRUE(Put().Code().empty());
  auto capability = Authorize("project");
  EXPECT_EQ(Authorize("project"), capability);
  const auto endpoint = JsonString(ready, "/execution_broker");
  Restart();
  EXPECT_EQ(JsonString(ready, "/execution_broker"), endpoint);
  EXPECT_NE(Broker(R::CatalogRequestT{}, capability)
                ->payload.AsReply()
                ->body.AsCatalog(),
            nullptr);
  const auto before = worker->Requests().size();
  auto denied = Broker(R::ProbeRequestT{}, "forged");
  EXPECT_EQ(denied->payload.AsReply()->body.AsFailure()->code,
            R::ErrorCode::Unauthorized);
  Revoke("project");
  Restart();
  auto revoked = Broker(R::ProbeRequestT{}, capability);
  EXPECT_EQ(revoked->payload.AsReply()->body.AsFailure()->code,
            R::ErrorCode::Unauthorized);
  EXPECT_EQ(worker->Requests().size(), before);
  auto next = Authorize("project");
  EXPECT_NE(next, capability);
  auto changed = Authorize("project", "replacement-directory");
  EXPECT_NE(changed, next);
  EXPECT_NE(
      Broker(R::CatalogRequestT{}, next)->payload.AsReply()->body.AsFailure(),
      nullptr);
}
TEST_F(BrokerFixture, PinnedRegistrationOwnsTheWorkerIdentity) {
  ASSERT_TRUE(Put().Code().empty());
  const auto capability = Authorize("project");
  const auto count = worker->Requests().size();
  auto changed = Broker(R::ProbeRequestT{}, capability);
  ASSERT_NE(changed->payload.AsReply()->body.AsCapabilities(), nullptr);
  EXPECT_EQ(changed->payload.AsReply()->body.AsCapabilities()->server_id,
            worker->server);
  EXPECT_EQ(worker->Requests().size(), count + 1);
  // Malformed frames cannot affect the daemon or trigger outbound requests.
  {
    Socket socket{
        ConnectUnix(JsonString(ready, "/execution_broker").substr(5))};
    std::string oversized(4, '\xff');
    Transfer(socket.fd, oversized, true);
    EXPECT_THROW(ReadMessage(socket.fd), std::runtime_error);
  }
  EXPECT_TRUE(List().Code().empty());
  EXPECT_EQ(worker->Requests().size(), count + 1);
}
}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
namespace {
TEST_F(BrokerFixture, WrongWorkerKeyIsRejectedBeforeAnyApplicationRequest) {
  peer_tls::Identity other(directory.path / "other-identity.pem");
  const auto before = worker->Requests().size();
  EXPECT_FALSE(Put(other.public_key()).Code().empty());
  EXPECT_EQ(worker->Requests().size(), before);
}
TEST_F(BrokerFixture, UnlistedHostKeyIsRejectedDuringTheWorkerHandshake) {
  peer_tls::Identity other(directory.path / "other-identity.pem");
  worker->Authorize(other.public_key());
  const auto before = worker->Requests().size();
  EXPECT_FALSE(Put().Code().empty());
  EXPECT_EQ(worker->Requests().size(), before);
}
TEST_F(BrokerFixture,
       RenameVerificationAndUnrelatedEditsPreserveProjectAuthorization) {
  ASSERT_TRUE(Put().Code().empty());
  const auto capability = Authorize("project");
  const auto background = Authorize("background-project");
  ASSERT_TRUE(Put({}, "worker", {}, false, "Renamed").Code().empty());
  ASSERT_TRUE(Put().Code().empty());
  ASSERT_TRUE(Put({}, "other").Code().empty());
  auto removed = Call(*host, H::RequestPayload::ExecutionServiceRemoveRequest,
                      [](Builder& b) {
                        return H::CreateExecutionServiceRemoveRequest(
                                   b, b.CreateString("other"))
                            .Union();
                      });
  ASSERT_TRUE(removed.Code().empty());
  Restart();
  for (const auto& grant : {capability, background}) {
    auto response = Broker(R::ProbeRequestT{}, grant);
    ASSERT_NE(response->payload.AsReply()->body.AsCapabilities(), nullptr);
  }
  EXPECT_EQ(Authorize("project"), capability);
}
TEST_F(BrokerFixture,
       ChangedRegistrationInvalidatesOnlyItsServiceBeforeDialing) {
  ASSERT_TRUE(Put().Code().empty());
  ASSERT_TRUE(Put({}, "other").Code().empty());
  const auto capability = Authorize("project");
  ASSERT_TRUE(
      Put({}, "other", "unix:/tmp/changed-worker.sock", false).Code().empty());
  auto response = Broker(R::ProbeRequestT{}, capability);
  ASSERT_NE(response->payload.AsReply()->body.AsCapabilities(), nullptr);
  ASSERT_TRUE(
      Put({}, "worker", "unix:/tmp/changed-worker.sock", false).Code().empty());
  auto count = worker->Requests().size();
  auto replaced = Broker(R::ProbeRequestT{}, capability);
  ASSERT_NE(replaced->payload.AsReply()->body.AsFailure(), nullptr);
  EXPECT_EQ(replaced->payload.AsReply()->body.AsFailure()->code,
            R::ErrorCode::RegistrationChanged);
  EXPECT_EQ(worker->Requests().size(), count);
}
TEST_F(BrokerFixture, RemovedAndReaddedRegistrationDoesNotReviveOldGrant) {
  ASSERT_TRUE(Put().Code().empty());
  const auto capability = Authorize("project");
  auto removed = Call(*host, H::RequestPayload::ExecutionServiceRemoveRequest,
                      [](Builder& b) {
                        return H::CreateExecutionServiceRemoveRequest(
                                   b, b.CreateString("worker"))
                            .Union();
                      });
  ASSERT_TRUE(removed.Code().empty());
  auto response = Broker(R::ProbeRequestT{}, capability);
  EXPECT_EQ(response->payload.AsReply()->body.AsFailure()->code,
            R::ErrorCode::RegistrationChanged);
  ASSERT_TRUE(Put().Code().empty());
  response = Broker(R::ProbeRequestT{}, capability);
  EXPECT_EQ(response->payload.AsReply()->body.AsFailure()->code,
            R::ErrorCode::RegistrationChanged);
}
TEST_F(BrokerFixture,
       ProbeRefreshesCapabilitiesWithoutReregistrationOrReauthorization) {
  worker->SetResourceLimits(false);
  ASSERT_TRUE(Put().Code().empty());
  const auto capability = Authorize("project");
  worker->SetResourceLimits(true);
  auto response = Broker(R::ProbeRequestT{}, capability);
  ASSERT_NE(response->payload.AsReply()->body.AsCapabilities(), nullptr);
  EXPECT_TRUE(
      response->payload.AsReply()->body.AsCapabilities()->resource_limits);
  auto listed = List();
  EXPECT_TRUE(listed.HostValue()
                  ->value_as_ExecutionServices()
                  ->services()
                  ->Get(1)
                  ->capabilities()
                  ->resource_limits());
  worker->SetResourceLimits(false);
  response = Broker(R::ProbeRequestT{}, capability);
  EXPECT_FALSE(
      response->payload.AsReply()->body.AsCapabilities()->resource_limits);
}

}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
namespace {
TEST_F(BrokerFixture, PublicPinCanBeRegisteredBeforeAuthorizingTheHost) {
  worker->Authorize("");
  ASSERT_TRUE(Put({}, "worker", {}, false).Code().empty());
  EXPECT_TRUE(worker->Requests().empty());
  auto reply = List();
  const auto* service =
      reply.HostValue()->value_as_ExecutionServices()->services()->Get(1);
  EXPECT_EQ(service->worker_public_key()->str(), worker->identity.public_key());
  EXPECT_TRUE(service->capabilities()->server_id()->str().empty());
  worker->Authorize(reply.HostValue()
                        ->value_as_ExecutionServices()
                        ->host_public_key()
                        ->str());
  EXPECT_TRUE(Put().Code().empty());
  EXPECT_EQ(worker->Requests().size(), 1u);
}
}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
namespace {
TEST_F(BrokerFixture, CatalogAndCancelProceedWhileWorkerRepliesAreBlocked) {
  ASSERT_TRUE(Put().Code().empty());
  const auto capability = Authorize("project");
  worker->HoldStatus();
  // Fill every ordinary slot with pending worker replies.
  std::array<Socket, 8> pending;
  for (auto& socket : pending)
    socket.fd = SendBroker(R::StatusRequestT{}, capability);
  ASSERT_TRUE(worker->WaitForHeldStatus(pending.size()));
  Socket queued{SendBroker(R::StatusRequestT{}, capability)};

  const auto catalog = Broker(R::CatalogRequestT{}, capability);
  ASSERT_NE(catalog->payload.AsReply()->body.AsCatalog(), nullptr);

  R::CancelRequestT cancel;
  cancel.fingerprint = std::string(64, 'd');
  const auto response = Broker(std::move(cancel), capability);
  ASSERT_NE(response->payload.AsReply(), nullptr);
  ASSERT_NE(response->payload.AsReply()->body.AsAck(), nullptr);
  const auto requests = worker->Requests();
  ASSERT_NE(requests.back().body.AsCancelRequest(), nullptr);
  EXPECT_EQ(requests.back().body.AsCancelRequest()->fingerprint,
            std::string(64, 'd'));
  // Only the original eight Status requests passed through. Closing one
  // core client releases its worker connection and admits the queued request.
  EXPECT_EQ(std::ranges::count_if(requests,
                                  [](const auto& req) {
                                    return req.body.AsStatusRequest() !=
                                           nullptr;
                                  }),
            pending.size());
  ::close(std::exchange(pending.front().fd, -1));
  EXPECT_TRUE(worker->WaitForHeldStatus(pending.size() + 1));
}

TEST_F(BrokerFixture, CardSecretsAreWriteOnlyAndBoundToTheProject) {
  const std::string sentinel = "CARD_SECRET_SENTINEL_5b1e9c";
  const std::string other = "CARD_SECRET_OTHER_PROJECT_7d24";
  const auto set = [&](const std::string& project, const std::string& name,
                       const std::string& value, bool remove) {
    return Call(
        *host, H::RequestPayload::CardSecretSetRequest, [&](Builder& b) {
          const auto v = value.empty()
                             ? flatbuffers::Offset<flatbuffers::String>()
                             : b.CreateString(value);
          return H::CreateCardSecretSetRequest(b, b.CreateString(project),
                                               b.CreateString(name), v, remove)
              .Union();
        });
  };
  const auto list = [&](const std::string& project) {
    return Call(
        *host, H::RequestPayload::CardSecretListRequest, [&](Builder& b) {
          return H::CreateCardSecretListRequest(b, b.CreateString(project))
              .Union();
        });
  };
  // Two project folders hostd registered as a2b3c4 and d5e6f7.
  for (const auto& [identity, id] :
       {std::pair{"first", "a2b3c4"}, std::pair{"second", "d5e6f7"}}) {
    const auto folder =
        directory.path / ("project-" + std::string(identity)) / ".tsugumi";
    std::filesystem::create_directories(folder);
    Write(folder / "project-id", id);
  }
  const auto names = [](const Reply& reply) {
    std::vector<std::string> out;
    const auto* list = reply.HostValue()->value_as_CardSecretList();
    if (list && list->names())
      for (const auto* name : *list->names()) out.push_back(name->str());
    return out;
  };
  const auto saved = set("a2b3c4", "database_password", sentinel, false);
  ASSERT_TRUE(saved.Code().empty()) << saved.View();
  EXPECT_EQ(names(saved), std::vector<std::string>{"database_password"});
  EXPECT_EQ(std::string(saved.bytes.begin(), saved.bytes.end()).find(sentinel),
            std::string::npos);
  ASSERT_TRUE(set("d5e6f7", "database_password", other, false).Code().empty());
  const auto listed = list("a2b3c4");
  EXPECT_EQ(names(listed), std::vector<std::string>{"database_password"});
  EXPECT_EQ(
      std::string(listed.bytes.begin(), listed.bytes.end()).find(sentinel),
      std::string::npos);
  EXPECT_TRUE(names(list("z9y8x7")).empty());
  // Names follow the Card definition's rule; a value and a project id are
  // required.
  EXPECT_FALSE(set("a2b3c4", "Database", "x", false).Code().empty());
  EXPECT_FALSE(set("a2b3c4", "anthropic:key", "x", false).Code().empty());
  EXPECT_FALSE(set("a2b3c4", "token", "", false).Code().empty());
  EXPECT_FALSE(set("../a2b3c4", "token", "x", false).Code().empty());
  EXPECT_FALSE(list("").Code().empty());

  using Values = std::vector<std::pair<std::string, std::string>>;
  const auto value = [&](const std::string& capability) {
    R::CardSecretsRequestT ask;
    ask.names = {"database_password", "not_set"};
    auto reply = Broker(ask, capability);
    const auto* secrets = reply->payload.AsReply()->body.AsCardSecrets();
    EXPECT_NE(secrets, nullptr);
    Values out;
    if (secrets)
      for (const auto& secret : secrets->secrets)
        out.emplace_back(secret->name, secret->value);
    return out;
  };
  const auto first = Authorize("first-core", "first");
  const auto second = Authorize("second-core", "second");
  // Project A cannot obtain project B's value under the same name: each core
  // receives only its own project's value.
  EXPECT_EQ(value(first), (Values{{"database_password", sentinel}}));
  EXPECT_EQ(value(second), (Values{{"database_password", other}}));
  // Provider keys share the store but are not Card secrets.
  R::CardSecretsRequestT provider;
  provider.names = {"desktop:fixture"};
  EXPECT_TRUE(Broker(provider, first)
                  ->payload.AsReply()
                  ->body.AsCardSecrets()
                  ->secrets.empty());
  R::CardSecretsRequestT ask;
  ask.names = {"database_password"};
  const auto denied = Broker(ask, "not-a-capability");
  const auto* failure = denied->payload.AsReply()->body.AsFailure();
  ASSERT_NE(failure, nullptr);
  EXPECT_EQ(failure->code, R::ErrorCode::Unauthorized);
  // A project folder without a hostd project id receives nothing.
  const auto unregistered = Broker(ask, Authorize("third-core", "third"));
  EXPECT_NE(unregistered->payload.AsReply()->body.AsFailure(), nullptr);

  EXPECT_TRUE(set("a2b3c4", "database_password", "", true).Code().empty());
  EXPECT_TRUE(value(first).empty());
  EXPECT_EQ(value(second), (Values{{"database_password", other}}));
}

TEST_F(BrokerFixture, FragmentedFramesWaitForTheCompleteRequest) {
  ASSERT_TRUE(Put().Code().empty());
  const auto capability = Authorize("project");
  const auto before = worker->Requests().size();
  R::RequestT request;
  request.body.Set(R::CatalogRequestT{});
  R::BrokerRequestT broker;
  broker.capability = capability;
  broker.request = std::make_unique<R::RequestT>(std::move(request));
  R::MessageT message;
  message.payload.Set(std::move(broker));
  auto bytes = W::encode(std::move(message));
  auto frame = W::length_prefix(static_cast<uint32_t>(bytes.size())) + bytes;
  Socket socket{ConnectUnix(JsonString(ready, "/execution_broker").substr(5))};
  auto first = frame.substr(0, 1);
  Transfer(socket.fd, first, true);
  auto rest = frame.substr(1);
  Transfer(socket.fd, rest, true);
  auto response = ReadMessage(socket.fd);
  ASSERT_NE(response->payload.AsReply(), nullptr);
  ASSERT_NE(response->payload.AsReply()->body.AsCatalog(), nullptr);
  EXPECT_EQ(response->payload.AsReply()->body.AsCatalog()->services.size(), 2u);
  EXPECT_EQ(worker->Requests().size(), before);
}
}  // namespace
}  // namespace tsugumi::test
