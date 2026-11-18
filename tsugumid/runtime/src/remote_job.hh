#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "tsugumi/workerd/protocol.hh"

namespace tsugumi {

struct RemoteTarget {
  std::string id;
  std::string native_system;
  std::vector<std::string> systems;
  bool available = false;
  bool resource_limits = false;
  uint32_t max_jobs = 0;
  uint64_t grade = 0;
  std::string unavailable_reason;
};

struct RemoteJobState {
  std::string state;
  std::string fingerprint;
  std::string result_sha256;
  std::string error;
  std::string transfer_name = {};
  uint64_t transfer_done = 0, transfer_total = 0;
  bool log_truncated = false;
};

struct RemoteProbe {
  std::string native_system;
  std::string instance;
  std::vector<std::string> systems;
  bool resource_limits;
  uint32_t max_jobs = 0, active_jobs = 0;
};

// An incomplete exchange is different from a response from the gateway.
// Callers may retry an incomplete exchange using the same job identity.
struct BrokerDisconnected : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct BrokerInstanceChanged : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct BrokerRequestRejected : std::runtime_error {
  using std::runtime_error::runtime_error;
};

using TransferProgress = std::function<void(uint64_t, uint64_t)>;

class RemoteJobClient {
 public:
  RemoteJobClient();
  ~RemoteJobClient();

  auto enabled() const -> bool { return !endpoint_.empty(); }
  auto stop() -> void { stopped_.store(true); }
  auto catalog() const -> std::vector<RemoteTarget>;
  auto probe(std::string_view service, std::string_view registered_server) const
      -> RemoteProbe;
  auto put_blob(std::string_view service, std::string_view instance,
                std::string_view job_id, std::string_view digest, uint64_t size,
                std::istream& contents,
                const std::atomic<bool>* cancel = nullptr,
                const TransferProgress& progress = {}) const -> void;
  auto get_blob(std::string_view service, std::string_view instance,
                std::string_view job_id, std::string_view digest,
                std::ostream& destination, uint64_t limit,
                std::optional<uint64_t> expected_size,
                const std::atomic<bool>* stop = nullptr,
                const TransferProgress& progress = {}) const -> void;
  auto submit(std::string_view service, std::string_view instance,
              std::string_view job_id, std::string_view spec) const
      -> RemoteJobState;
  auto status(std::string_view service, std::string_view instance,
              std::string_view job_id) const -> RemoteJobState;
  auto cancel(std::string_view service, std::string_view instance,
              std::string_view job_id, std::string_view fingerprint) const
      -> RemoteJobState;

  auto release(std::string_view service, std::string_view instance,
               std::string_view job_id, std::string_view fingerprint) const
      -> void;

 private:
  auto request(Tsugumi::RemoteWorker::RequestT message,
               const std::atomic<bool>* cancel = nullptr) const
      -> Tsugumi::RemoteWorker::ReplyT;
  const std::string core_life_id_;
  std::string endpoint_;
  std::filesystem::path capability_file_;
  std::atomic<bool> stopped_{false};
};

}  // namespace tsugumi
