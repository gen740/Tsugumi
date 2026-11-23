#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "tsugumi/workerd/protocol.hh"
#include "tsugumi/workerd/values.hh"

namespace tsugumi {

struct RemoteTarget {
  std::string id;
  std::string native_system;
  std::vector<std::string> systems;
  std::string server_id;
  bool available = false;
  bool resource_limits = false;
  uint32_t max_jobs = 0;
  uint64_t grade = 0;
  std::string unavailable_reason;
  bool namespace_isolation = false;
  bool gui = false;
};

struct RemoteJobState {
  std::string state;
  std::string fingerprint;
  std::string result_sha256;
  std::string error;
  std::string transfer_name = {};
  uint64_t transfer_done = 0, transfer_total = 0;
  bool log_truncated = false;
  // A status reply's slice of the job log: its offset, the log's size then,
  // and the bytes from that offset.
  uint64_t log_offset = 0, log_size = 0;
  std::string log = {};
  // The running job waits for its Card secrets (JobSecretsRequest).
  bool secrets_wanted = false;
  std::vector<WorkerDisplayData> displays = {};
  bool display_pending = false;
  uint64_t display_sequence = 0;
  bool view_input_ready = false;
  std::map<std::string, uint64_t> view_input_credits = {};
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
  auto stop() -> void { stopped_.request_stop(); }
  auto catalog(std::stop_token stop = {}) const -> std::vector<RemoteTarget>;
  auto probe(std::string_view service, std::string_view registered_server,
             std::stop_token stop = {}) const -> RemoteProbe;
  auto put_blob(std::string_view service, std::string_view instance,
                std::string_view job_id, std::string_view digest, uint64_t size,
                std::istream& contents, std::stop_token cancel = {},
                const TransferProgress& progress = {}) const -> void;
  auto get_blob(std::string_view service, std::string_view instance,
                std::string_view job_id, std::string_view digest,
                std::ostream& destination, uint64_t limit,
                std::optional<uint64_t> expected_size,
                std::stop_token stop = {},
                const TransferProgress& progress = {}) const -> void;
  auto submit(std::string_view service, std::string_view instance,
              std::string_view job_id, std::string_view spec,
              std::stop_token stop) const -> RemoteJobState;
  auto send_view(std::string_view service, std::string_view instance,
                 std::string_view job_id, const WorkerDisplayData& display,
                 uint64_t offset, std::stop_token stop) const -> void;
  auto status(std::string_view service, std::string_view instance,
              std::string_view job_id, uint64_t log_offset,
              std::stop_token stop, uint64_t display_after = 0,
              const std::map<std::string, uint64_t>& credits = {}) const
      -> RemoteJobState;
  // With a log cursor, the reply also carries the job log from there; only
  // the submitting core life may read it.
  auto cancel(std::string_view service, std::string_view instance,
              std::string_view job_id, std::string_view fingerprint,
              std::optional<uint64_t> log_offset = std::nullopt) const
      -> RemoteJobState;

  auto release(std::string_view service, std::string_view instance,
               std::string_view job_id, std::string_view fingerprint) const
      -> void;
  // The values hostd holds for these Card secret names, read just before an
  // invocation and kept only in memory. Names that are not set are absent.
  auto card_secrets(const std::vector<std::string>& names,
                    std::stop_token stop) const
      -> std::vector<std::pair<std::string, std::string>>;
  // Hands a running job that waits for them its Card secrets, once. The
  // values are cleared from `secrets` whether or not the worker accepts them.
  auto deliver_secrets(
      std::string_view service, std::string_view instance, std::string_view job,
      std::string_view fingerprint,
      std::vector<std::pair<std::string, std::string>>& secrets,
      std::stop_token stop) const -> void;

 private:
  auto request(Tsugumi::RemoteWorker::RequestT message,
               std::stop_token cancel = {}) const
      -> Tsugumi::RemoteWorker::ReplyT;
  const std::string core_life_id_;
  std::string endpoint_;
  std::filesystem::path capability_file_;
  std::stop_source stopped_;
};

}  // namespace tsugumi
