// Runs Card invocations on remote execution daemons as durable jobs: the
// derivation, inputs and call are uploaded, the job is registered in the
// history before it is submitted, and its result is imported when it ends.
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

  // Every activation transfers its derivation closure for the remote worker to
  // realise.
  auto prepare(const RemoteTarget& target, const Node& node,
               const std::filesystem::path& directory, BuildRequest& build)
      -> std::unique_ptr<PreparedExecution>;

  // Retries cancellation/release acknowledgements, never result retrieval.
  auto release_completed() -> void;

 private:
  Service& service_;
  RemoteJobClient& client_;
};

}  // namespace tsugumi
