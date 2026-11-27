// Runs Card invocations as durable remote jobs. Derivations go through a
// binary cache. Invocation inputs and results go through the execution
// broker. The core records the job in history before submission.
// A job belongs only to the core process that started it.
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "execution_service.hh"
#include "history.hh"
#include "remote_job.hh"

namespace tsugumi {

class RemoteExecutionService final {
 public:
  RemoteExecutionService(Service& service, RemoteJobClient& client);

  // Publish the derivation closure for the worker to realise.
  auto prepare(const RemoteTarget& target, const Node& node,
               const std::filesystem::path& directory, BuildRequest& build)
      -> std::unique_ptr<PreparedExecution>;

  // Retries cancellation/release acknowledgements, never result retrieval.
  auto release_completed() -> void;
  auto publish_local_build(const WorkerDerivation& derivation,
                           bool push_outputs, std::stop_token stop)
      -> std::optional<bool>;

 private:
  Service& service_;
  RemoteJobClient& client_;
};

}  // namespace tsugumi
