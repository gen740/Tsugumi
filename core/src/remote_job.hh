#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tsugumi {

struct RemoteTarget {
  std::string id;
  std::string native_system;
  std::vector<std::string> systems;
  bool available;
};

struct RemoteJobState {
  std::string state;
  std::string fingerprint;
  std::string result_sha256;
  std::string error;
};

struct RemoteProbe {
  std::string instance;
  std::vector<std::string> systems;
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

class RemoteJobClient {
 public:
  RemoteJobClient();

  auto enabled() const -> bool { return !endpoint_.empty(); }
  auto catalog() const -> std::vector<RemoteTarget>;
  auto probe(std::string_view service) const -> RemoteProbe;
  auto put_blob(std::string_view service, std::string_view instance, std::string_view digest, uint64_t size,
                std::istream& contents, const std::atomic<bool>* cancel = nullptr) const -> void;
  auto get_blob(std::string_view service, std::string_view instance, std::string_view digest,
                std::ostream& destination, uint64_t limit, const std::atomic<bool>* stop = nullptr) const -> void;
  auto submit(std::string_view service, std::string_view instance, std::string_view job_id, std::string_view spec) const -> RemoteJobState;
  auto status(std::string_view service, std::string_view instance, std::string_view job_id) const -> RemoteJobState;
  auto cancel(std::string_view service, std::string_view instance, std::string_view job_id, std::string_view fingerprint) const -> RemoteJobState;

 private:
  struct Reply;
  auto request(std::string_view operation, std::string_view service = {}, std::string_view instance = {}, std::string_view job_id = {},
               std::string_view digest = {}, uint64_t size = 0, std::istream* body = nullptr,
               std::ostream* destination = nullptr, uint64_t limit = 0,
               const std::atomic<bool>* cancel = nullptr) const -> Reply;
  auto connect() const -> int;

  std::string endpoint_;
  std::filesystem::path token_file_;
};

}  // namespace tsugumi
