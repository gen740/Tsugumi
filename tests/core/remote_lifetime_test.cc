#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#include "journal.hh"
#include "remote_execution_service.hh"
#include "service.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi {
namespace {
namespace W = workerd;
namespace R = Tsugumi::RemoteWorker;
using namespace std::chrono_literals;

class ScopedEnvironment {
 public:
  ScopedEnvironment(const char* name, const std::string& value) : name_(name) {
    if (const auto* old = std::getenv(name)) previous_ = old;
    ::setenv(name, value.c_str(), 1);
  }
  ~ScopedEnvironment() {
    if (previous_)
      ::setenv(name_, previous_->c_str(), 1);
    else
      ::unsetenv(name_);
  }

 private:
  const char* name_;
  std::optional<std::string> previous_;
};

// Only the broker transport/dispatch is a double. The core and the
// listener's journal (including touch-before-renew expiry) are production code.
class JournalBroker : public testing::Test {
 protected:
  auto SetUp() -> void override {
    previous_umask_ =
        ::umask(0077);  // Match the real listener's private state.
    root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
           ("lease-" + crypto::uuid4());
    std::filesystem::create_directories(root / "journal");
    std::filesystem::permissions(root / "journal",
                                 std::filesystem::perms::owner_all);
    journal = std::make_unique<W::Journal>(root / "journal");
    fs::write_atomic(root / "capability", "test-capability", 0600);
    endpoint = std::make_unique<ScopedEnvironment>(
        "TSUGUMI_EXECUTION_BROKER", "unix:" + (root / "broker").string());
    capability = std::make_unique<ScopedEnvironment>(
        "TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE",
        (root / "capability").string());
    socket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(socket_, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = (root / "broker").string();
    ASSERT_LT(path.size(), sizeof address.sun_path);
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    ASSERT_EQ(
        ::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof address),
        0);
    ASSERT_EQ(::listen(socket_, 16), 0);
    thread_ = std::jthread([this] { Serve(); });
  }
  auto TearDown() -> void override {
    stopping_ = true;
    ::shutdown(socket_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    ::close(socket_);
    EXPECT_FALSE(failed_);
    std::filesystem::remove_all(root);
    ::umask(previous_umask_);
  }
  auto WaitForProbes(size_t count) -> bool {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return probes_ >= count; });
    return true;
  }
  auto Submit(RemoteJobClient& client, const std::string& job) -> std::string {
    std::istringstream input("input");
    const auto digest = crypto::sha256_hex("input");
    client.put_blob("worker", journal->instance(), job, digest, 5, input);
    R::JobSpecT spec;
    spec.system = "x86_64-linux";
    spec.derivation = "/nix/store/00000000000000000000000000000000-test.drv";
    spec.output = "/nix/store/00000000000000000000000000000000-test";
    spec.invocation_sha256 = digest;
    spec.input_blobs = {digest};
    spec.log_limit_bytes = 8u << 20;
    flatbuffers::FlatBufferBuilder b;
    b.Finish(R::JobSpec::Pack(b, &spec));
    const std::string bytes(reinterpret_cast<const char*>(b.GetBufferPointer()),
                            b.GetSize());
    EXPECT_EQ(client.submit("worker", journal->instance(), job, bytes, {}).state,
              "queued");
    return crypto::sha256_hex(bytes);
  }
  std::filesystem::path root;
  std::unique_ptr<W::Journal> journal;
  const std::string server = std::string(64, 'a');

 private:
  static auto Read(int fd, size_t size) -> std::string {
    std::string bytes(size, '\0');
    size_t offset = 0;
    while (offset < size) {
      const auto n = ::read(fd, bytes.data() + offset, size - offset);
      if (n <= 0) throw std::runtime_error("broker read failed");
      offset += static_cast<size_t>(n);
    }
    return bytes;
  }
  auto Serve() -> void {
    while (!stopping_) {
      pollfd ready{socket_, POLLIN, 0};
      if (::poll(&ready, 1, 100) <= 0) continue;
      if (stopping_) return;
      const int fd = ::accept(socket_, nullptr, nullptr);
      if (fd < 0) return;
      struct Close {
        int fd;
        ~Close() { ::close(fd); }
      } close{fd};
      const timeval timeout{3, 0};
      ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
      try {
        const auto size = W::frame_length(Read(fd, 4));
        const auto message = W::decode(Read(fd, size));
        const auto* broker = message->payload.AsBrokerRequest();
        if (!broker || broker->capability != "test-capability" ||
            !broker->request)
          throw std::runtime_error("invalid broker request");
        const auto& q = *broker->request;
        R::MessageT response;
        if (q.body.AsProbeRequest()) {
          // Advance lease time explicitly; machine load cannot expire it.
          lease_time_ += 1150;
          R::CapabilitiesT caps;
          caps.server_id = server;
          caps.instance_id = journal->instance();
          caps.native_system = "x86_64-linux";
          caps.systems = {caps.native_system};
          caps.max_jobs = 1;
          caps.worker_contract = W::kContract;
          response = W::reply(std::move(caps));
        } else if (const auto* put = q.body.AsPutBlobRequest()) {
          journal->put(q.job_id, *put, q.core_life_id);
          response = W::reply(R::AckT{});
        } else if (const auto* submit = q.body.AsSubmitRequest()) {
          const std::string_view bytes(
              reinterpret_cast<const char*>(submit->spec.data()),
              submit->spec.size());
          response =
              W::reply(journal
                           ->submit(q.job_id, bytes, *W::parse_spec(bytes),
                                    q.core_life_id)
                           .status());
        } else if (const auto* cancel = q.body.AsCancelRequest()) {
          response =
              W::reply(journal->cancel(q.job_id, cancel->fingerprint).status());
        } else if (const auto* release = q.body.AsReleaseRequest()) {
          journal->release(q.job_id, release->fingerprint);
          response = W::reply(R::AckT{});
        } else
          throw std::runtime_error("unexpected broker request");
        auto bytes = W::encode(std::move(response));
        bytes = W::length_prefix(static_cast<uint32_t>(bytes.size())) + bytes;
        // The live client reads the whole response before it closes this
        // socket.
        size_t offset = 0;
        while (offset < bytes.size()) {
          const auto n =
              ::write(fd, bytes.data() + offset, bytes.size() - offset);
          if (n <= 0) throw std::runtime_error("broker write failed");
          offset += static_cast<size_t>(n);
        }
        if (q.body.AsProbeRequest()) {
          {
            std::lock_guard lock(mutex_);
            ++probes_;
          }
          changed_.notify_all();
        }
      } catch (const std::exception&) {
        failed_ = true;
      }
    }
  }
  int socket_ = -1;
  mode_t previous_umask_ = 0;
  std::jthread thread_;
  std::atomic<bool> stopping_{false}, failed_{false};
  std::mutex mutex_;
  std::condition_variable changed_;
  size_t probes_ = 0;
  uint64_t lease_time_ = W::now_ms();
  std::unique_ptr<ScopedEnvironment> endpoint, capability;
};

TEST_F(JournalBroker, InterruptedCleanupKeepsUncollectedInputsResultsAndLogs) {
  RemoteJobClient previous;
  const std::string completed(64, '2'), queued(64, '3'), collected(64, '4');
  std::map<std::string, std::string> fingerprints;
  for (const auto& job : {completed, queued, collected}) {
    fingerprints[job] = Submit(previous, job);
    fs::write_atomic(journal->job_path(job) / "work/result/output", "output",
                     0600);
    fs::write_atomic(journal->job_path(job) / "supervisor.log", "log", 0600);
  }
  journal->claim(completed, 1, 1, "boot");
  journal->finish(completed, R::JobState::Succeeded,
                  crypto::sha256_hex("result"), {});
  ServiceOptions options;
  options.root = root / "project";
  std::filesystem::create_directories(options.root);
  fs::write_atomic(
      options.root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  options.scratch_dir = root / "scratch";
  options.run_scratch_dir = root / "outputs";
  Service current(options);
  current.open();
  current.history().write_sync([&](sql::Db& db) {
    for (const auto& [job, fingerprint] : fingerprints)
      db.prepare("INSERT INTO remote_releases VALUES(?,?,?,?,?)")
          .bind(1, job)
          .bind(2, "worker")
          .bind(3, journal->instance())
          .bind(4, fingerprint)
          .bind(5, job == collected)
          .run();
  });
  RemoteJobClient replacement;
  RemoteExecutionService cleanup(current, replacement);
  cleanup.release_completed();
  for (const auto& job : {completed, queued}) {
    EXPECT_TRUE(std::filesystem::exists(journal->job_path(job) / "blobs" /
                                        crypto::sha256_hex("input")));
    EXPECT_EQ(fs::read_file(journal->job_path(job) / "work/result/output"),
              "output");
    EXPECT_EQ(fs::read_file(journal->job_path(job) / "supervisor.log"), "log");
    EXPECT_FALSE(journal->claim(job, 2, 2, "boot"));
  }
  EXPECT_EQ(journal->status(completed).state, R::JobState::Succeeded);
  EXPECT_EQ(journal->status(queued).state, R::JobState::Cancelled);
  EXPECT_EQ(journal->status(collected).state, R::JobState::Released);
  EXPECT_FALSE(std::filesystem::exists(journal->job_path(collected) / "blobs"));
  current.history().read([](sql::Db& db) {
    EXPECT_FALSE(db.prepare("SELECT 1 FROM remote_releases").step());
  });
}
}  // namespace
}  // namespace tsugumi
