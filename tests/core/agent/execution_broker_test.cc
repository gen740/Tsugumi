// Public Host management RPC and independent remote-worker wire peer.
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "fixture.hh"
#include "tsugumi/workerd/protocol.hh"

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
    const auto n = send
                       ? ::write(fd, data.data() + offset, data.size() - offset)
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
  timeval timeout{3, 0};
  ::setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  const auto fd = socket.fd;
  socket.fd = -1;
  return fd;
}
class WorkerPeer {
 public:
  explicit WorkerPeer(const std::filesystem::path& path)
      : path_(path.string()) {
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
        timeval timeout{3, 0};
        ::setsockopt(peer.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof timeout);
        try {
          auto message = ReadMessage(peer.fd);
          auto* q = message->payload.AsRequest();
          if (!q) continue;
          std::lock_guard lock(mutex_);
          requests_.push_back(*q);
          if (q->token != token) {
            SendMessage(peer.fd, W::failure(R::ErrorCode::Unauthorized,
                                            "fixture worker rejected token"));
          } else if (q->body.AsProbeRequest()) {
            R::CapabilitiesT c;
            c.server_id = server;
            c.instance_id = "live-instance";
            c.native_system = "x86_64-linux";
            c.systems = {c.native_system};
            c.max_jobs = 4;
            c.resource_limits = true;
            c.worker_contract = contract;
            SendMessage(peer.fd, W::reply(std::move(c)));
          } else if (auto* put = q->body.AsPutBlobRequest()) {
            R::BlobChunkT chunk;
            chunk.digest = put->digest;
            chunk.total = put->total;
            chunk.offset = put->offset;
            chunk.bytes = put->bytes;
            SendMessage(peer.fd, W::reply(std::move(chunk)));
          } else if (q->body.AsStatusRequest()) {
            // Simulate a submit/status whose response was lost.
          } else
            SendMessage(peer.fd, W::reply(R::AckT{}));
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
  std::string token = std::string(64, 'a');
  std::string server = std::string(64, 'b');
  uint32_t contract = W::kContract;

 private:
  std::string path_;
  int socket_;
  std::atomic<bool> stopping_ = false;
  std::mutex mutex_;
  std::vector<R::RequestT> requests_;
  std::jthread thread_;
};
class BrokerFixture : public AgentFixture {
 protected:
  auto SetUp() -> void override {
    AgentFixture::SetUp();
    worker = std::make_unique<WorkerPeer>(directory.path / "worker.sock");
  }
  auto Put(std::string token = std::string(64, 'a'), std::string id = "worker",
           std::string endpoint = {}) -> Reply {
    return Call(
        *host, H::RequestPayload::ExecutionServicePutRequest, [&](Builder& b) {
          return H::CreateExecutionServicePutRequest(
                     b, b.CreateString(id), b.CreateString("Test worker"),
                     b.CreateString(
                         endpoint.empty()
                             ? "unix:" +
                                   (directory.path / "worker.sock").string()
                             : endpoint),
                     token.empty() ? 0 : b.CreateString(token))
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
    const auto token = Read(control / "execution-broker-token");
    EXPECT_EQ(
        std::string(response.bytes.begin(), response.bytes.end()).find(token),
        std::string::npos);
    struct stat st{};
    ::stat((control / "execution-broker-token").c_str(), &st);
    EXPECT_EQ(st.st_mode & 0077, 0);
    return token;
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
  auto Broker(T body, const std::string& token,
              std::string server = std::string(64, 'b'))
      -> std::unique_ptr<R::MessageT> {
    Socket socket{
        ConnectUnix(JsonString(ready, "/execution_broker").substr(5))};
    R::RequestT request;
    request.token = token;
    request.service = "worker";
    request.server_id = server;
    request.project_scope = "SPOOFED";
    request.instance_id = "instance";
    request.job_id = "job";
    request.body.Set(std::move(body));
    R::MessageT message;
    message.payload.Set(std::move(request));
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
      List().HostValue()->value_as_ExecutionServices()->services()->size(), 0u);
  auto response = Put();
  ASSERT_TRUE(response.Code().empty());
  const auto* values =
      response.HostValue()->value_as_ExecutionServices()->services();
  ASSERT_EQ(values->size(), 1u);
  EXPECT_EQ(values->Get(0)->capabilities()->server_id()->str(),
            std::string(64, 'b'));
  EXPECT_EQ(std::string(response.bytes.begin(), response.bytes.end())
                .find(worker->token),
            std::string::npos);
  auto requests = worker->Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_TRUE(requests[0].project_scope.empty());
  Restart();
  EXPECT_EQ(
      List().HostValue()->value_as_ExecutionServices()->services()->size(), 1u);
  EXPECT_TRUE(Put("").Code().empty());
  EXPECT_FALSE(Put(std::string(64, 'c')).Code().empty());
  EXPECT_EQ(
      List().HostValue()->value_as_ExecutionServices()->services()->size(), 1u);
  const auto count = worker->Requests().size();
  EXPECT_FALSE(Put(worker->token, "local").Code().empty());
  EXPECT_FALSE(Put(worker->token, "bad ID").Code().empty());
  EXPECT_FALSE(
      Put(worker->token, "worker", "tcp://example.com:443").Code().empty());
  EXPECT_EQ(worker->Requests().size(), count);
  const auto removed = Call(
      *host, H::RequestPayload::ExecutionServiceRemoveRequest, [](Builder& b) {
        return H::CreateExecutionServiceRemoveRequest(b,
                                                      b.CreateString("worker"))
            .Union();
      });
  EXPECT_EQ(
      removed.HostValue()->value_as_ExecutionServices()->services()->size(),
      0u);
  Restart();
  EXPECT_EQ(
      List().HostValue()->value_as_ExecutionServices()->services()->size(), 0u);
}
TEST_F(BrokerFixture,
       RelayOverwritesScopeAndWorkerCredentialAndPreservesChunkBytes) {
  ASSERT_TRUE(Put().Code().empty());
  auto a = Authorize("a"), b = Authorize("b", "different-directory");
  EXPECT_NE(a, b);
  auto catalog = Broker(R::CatalogRequestT{}, a);
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
  EXPECT_EQ(worker->Requests().back().token, worker->token);
  Broker(put, b);
  EXPECT_NE(worker->Requests().back().project_scope, scope);
  // Same filesystem identity under a host alias shares the isolation scope.
  Broker(put, Authorize("alias"));
  EXPECT_EQ(worker->Requests().back().project_scope, scope);
  EXPECT_THROW(Broker(R::StatusRequestT{}, a), std::runtime_error);
}
TEST_F(BrokerFixture, AuthorizationSurvivesRestartAndRevocationIsDurable) {
  ASSERT_TRUE(Put().Code().empty());
  auto token = Authorize("project");
  EXPECT_EQ(Authorize("project"), token);
  const auto endpoint = JsonString(ready, "/execution_broker");
  Restart();
  EXPECT_EQ(JsonString(ready, "/execution_broker"), endpoint);
  EXPECT_NE(
      Broker(R::CatalogRequestT{}, token)->payload.AsReply()->body.AsCatalog(),
      nullptr);
  const auto before = worker->Requests().size();
  auto denied = Broker(R::ProbeRequestT{}, "forged");
  EXPECT_EQ(denied->payload.AsReply()->body.AsFailure()->code,
            R::ErrorCode::Unauthorized);
  Revoke("project");
  Restart();
  auto revoked = Broker(R::ProbeRequestT{}, token);
  EXPECT_EQ(revoked->payload.AsReply()->body.AsFailure()->code,
            R::ErrorCode::Unauthorized);
  EXPECT_EQ(worker->Requests().size(), before);
  auto next = Authorize("project");
  EXPECT_NE(next, token);
  auto changed = Authorize("project", "replacement-directory");
  EXPECT_NE(changed, next);
  EXPECT_NE(
      Broker(R::CatalogRequestT{}, next)->payload.AsReply()->body.AsFailure(),
      nullptr);
}
TEST_F(BrokerFixture, StaleRegistrationIsRejectedBeforeContactingTheWorker) {
  ASSERT_TRUE(Put().Code().empty());
  const auto token = Authorize("project");
  const auto count = worker->Requests().size();
  auto changed = Broker(R::StatusRequestT{}, token, std::string(64, 'c'));
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
