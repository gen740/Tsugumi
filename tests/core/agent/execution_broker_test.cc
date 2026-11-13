// Public Host management RPC and independent remote-worker wire peer.
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "fixture.hh"
#include "tsugumi/workerd/protocol.hh"
#include "util/peer_tls.hh"

namespace tsugumi::test {
namespace {
namespace R = Tsugumi::RemoteWorker;
namespace W = tsugumi::workerd;
struct Socket {
  int fd;
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
            c.instance_id = "live-instance";
            c.native_system = "x86_64-linux";
            c.systems = {c.native_system};
            c.max_jobs = 4;
            c.resource_limits = true;
            c.worker_contract = contract;
            respond(W::reply(std::move(c)));
          } else if (auto* put = q->body.AsPutBlobRequest()) {
            R::BlobChunkT chunk;
            chunk.digest = put->digest;
            chunk.total = put->total;
            chunk.offset = put->offset;
            chunk.bytes = put->bytes;
            respond(W::reply(std::move(chunk)));
          } else if (q->body.AsStatusRequest()) {
            // Simulate a submit/status whose response was lost.
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
    ::close(socket_);
    ::unlink(path_.c_str());
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
  std::string server = std::string(64, 'b');
  uint32_t contract = W::kContract;

 private:
  std::string path_;
  std::filesystem::path authorized_;
  peer_tls::Context tls_;
  int socket_;
  std::atomic<bool> stopping_ = false;
  std::mutex mutex_;
  std::vector<R::RequestT> requests_;
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
           std::string endpoint = {}, bool verify = true) -> Reply {
    return Call(
        *host, H::RequestPayload::ExecutionServicePutRequest, [&](Builder& b) {
          return H::CreateExecutionServicePutRequest(
                     b, b.CreateString(id), b.CreateString("Test worker"),
                     b.CreateString(
                         endpoint.empty()
                             ? "unix:" +
                                   (directory.path / "worker.sock").string()
                             : endpoint),
                     b.CreateString(key.empty() ? worker->identity.public_key()
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
  auto Broker(T body, const std::string& capability,
              std::string server = std::string(64, 'b'))
      -> std::unique_ptr<R::MessageT> {
    Socket socket{
        ConnectUnix(JsonString(ready, "/execution_broker").substr(5))};
    R::RequestT request;
    request.service = "worker";
    request.server_id = server;
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
    ::shutdown(socket.fd, SHUT_WR);
    return ReadMessage(socket.fd);
  }
  auto Restart() -> void {
    host.reset();
    human.reset();
    external.reset();
    agent.Stop();
    StartAgent();
  }
  std::unique_ptr<WorkerPeer> worker;
};
TEST_F(BrokerFixture, RegistrationProbesPersistsAndNeverReturnsCredentials) {
  EXPECT_EQ(
      ListSize(List().HostValue()->value_as_ExecutionServices()->services()),
      0u);
  auto response = Put();
  ASSERT_TRUE(response.Code().empty());
  const auto* values =
      response.HostValue()->value_as_ExecutionServices()->services();
  ASSERT_EQ(ListSize(values), 1u);
  EXPECT_EQ(values->Get(0)->capabilities()->server_id()->str(),
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
      1u);
  EXPECT_TRUE(Put("").Code().empty());
  EXPECT_FALSE(Put("not-a-key").Code().empty());
  EXPECT_EQ(
      ListSize(List().HostValue()->value_as_ExecutionServices()->services()),
      1u);
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
      0u);
  Restart();
  EXPECT_EQ(
      ListSize(List().HostValue()->value_as_ExecutionServices()->services()),
      0u);
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
  ASSERT_EQ(catalog->payload.AsReply()->body.AsCatalog()->services.size(), 1u);
  EXPECT_EQ(catalog->payload.AsReply()->body.AsCatalog()->services[0]->id,
            "worker");
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
  EXPECT_EQ(scope.size(), 64u);
  Broker(put, b);
  EXPECT_NE(worker->Requests().back().project_scope, scope);
  // Same filesystem identity under a host alias shares the isolation scope.
  Broker(put, Authorize("alias"));
  EXPECT_EQ(worker->Requests().back().project_scope, scope);
  EXPECT_THROW(Broker(R::StatusRequestT{}, a), std::runtime_error);
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
TEST_F(BrokerFixture, StaleRegistrationIsRejectedBeforeContactingTheWorker) {
  ASSERT_TRUE(Put().Code().empty());
  const auto capability = Authorize("project");
  const auto count = worker->Requests().size();
  auto changed = Broker(R::StatusRequestT{}, capability, std::string(64, 'c'));
  EXPECT_EQ(changed->payload.AsReply()->body.AsFailure()->code,
            R::ErrorCode::RegistrationChanged);
  EXPECT_EQ(worker->Requests().size(), count);
  // Malformed frames cannot affect the daemon or trigger outbound requests.
  {
    Socket socket{
        ConnectUnix(JsonString(ready, "/execution_broker").substr(5))};
    std::string oversized(4, '\xff');
    Transfer(socket.fd, oversized, true);
    ::shutdown(socket.fd, SHUT_WR);
    EXPECT_THROW(ReadMessage(socket.fd), std::runtime_error);
  }
  EXPECT_TRUE(List().Code().empty());
  EXPECT_EQ(worker->Requests().size(), count);
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
TEST_F(
    BrokerFixture,
    ReplacedAndRemovedRegistrationsInvalidateExistingCapabilitiesBeforeDialing) {
  ASSERT_TRUE(Put().Code().empty());
  const auto capability = Authorize("project");
  ASSERT_TRUE(Put().Code().empty());
  auto count = worker->Requests().size();
  auto replaced = Broker(R::ProbeRequestT{}, capability);
  ASSERT_NE(replaced->payload.AsReply()->body.AsFailure(), nullptr);
  EXPECT_EQ(replaced->payload.AsReply()->body.AsFailure()->code,
            R::ErrorCode::RegistrationChanged);
  EXPECT_EQ(worker->Requests().size(), count);
  const auto fresh = Authorize("project");
  const auto removed = Call(
      *host, H::RequestPayload::ExecutionServiceRemoveRequest, [](Builder& b) {
        return H::CreateExecutionServiceRemoveRequest(b,
                                                      b.CreateString("worker"))
            .Union();
      });
  ASSERT_TRUE(removed.Code().empty());
  auto refused = Broker(R::ProbeRequestT{}, fresh);
  EXPECT_EQ(refused->payload.AsReply()->body.AsFailure()->code,
            R::ErrorCode::RegistrationChanged);
  EXPECT_EQ(worker->Requests().size(), count);
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
      reply.HostValue()->value_as_ExecutionServices()->services()->Get(0);
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
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  auto rest = frame.substr(1);
  Transfer(socket.fd, rest, true);
  ::shutdown(socket.fd, SHUT_WR);
  auto response = ReadMessage(socket.fd);
  ASSERT_NE(response->payload.AsReply(), nullptr);
  ASSERT_NE(response->payload.AsReply()->body.AsCatalog(), nullptr);
  EXPECT_EQ(response->payload.AsReply()->body.AsCatalog()->services.size(), 1u);
  EXPECT_EQ(worker->Requests().size(), before);
}
}  // namespace
}  // namespace tsugumi::test
