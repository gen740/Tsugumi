// Native data shared by the scheduler, the history store and RPC encoding.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "tsugumi/document/document.hh"
#include "tsugumi/document/types.hh"
#include "tsugumi/protocol/tsugumi_enums_generated.h"
#include "tsugumi/workerd/module_source.hh"
#include "tsugumi/workerd/values.hh"

namespace tsugumi {

struct RunStatusData {
  std::optional<std::string> run_id;
  RunState state = RunState::idle;
  std::optional<uint64_t> rev;
  std::optional<uint64_t> execution_rev;
  std::optional<int64_t> accepted_at;
  std::optional<int64_t> started_at;
  std::optional<int64_t> ended_at;
  std::optional<FailureData> failure;
  std::optional<std::string>
      commit;  // the project's version the run started from
  bool partial_run = false;
  bool uses_partial_result = false;
  bool terminal() const {
    return state == RunState::succeeded || state == RunState::failed ||
           state == RunState::cancelled;
  }
};

struct RunSummaryData {
  RunStatusData status;
  std::string source_hash;
  CardPath target;
  bool propagate = true;
};

struct UpstreamRef {
  CardPath card;
  std::string port;
};

struct WaitingInputData {
  std::string port;
  std::vector<UpstreamRef> upstream;
};

struct StageTimeData {
  Phase phase = Phase::unspecified;
  StageOutcome outcome = StageOutcome::running;
  int64_t start = 0;
  std::optional<int64_t> end;
};

// A derivation the invocation's Nix build is building now (kept in memory).
struct BuildActivityData {
  std::string derivation;
  int64_t start = 0;
};

struct BuildEnvironmentData {
  std::string id, derivation, manifest, system;
  std::vector<std::string> paths;
};

struct InvocationSummaryData {
  std::string run_id;
  CardPath card;
  uint64_t invocation = 0;
  CardState state = CardState::not_started;
  std::optional<Phase> phase;
  std::optional<int64_t> started_at;
  std::optional<int64_t> ended_at;
  std::optional<uint64_t> duration_ms;
  std::optional<FailureData> failure;
  std::vector<WaitingInputData> waiting;
  std::optional<bool> build_cached;
  std::optional<uint64_t> log_truncated_at_bytes;
  std::vector<StageTimeData> stages;
  std::vector<BuildActivityData> building;
  std::optional<bool> worker_reused;
  std::vector<std::string> outputs_sent;
  std::optional<int64_t> ready_at;
  // The program the kernel ran the Card's binaries with, for a Card built for
  // another system (binfmt_misc); "" when it ran natively.
  std::string emulator;
  std::string environment_id;
  std::string
      params_json;  // the invocation's parameters, not the current document
  std::string execution_service;
  std::string transfer_name, transfer_direction;
  uint64_t transfer_done = 0, transfer_total = 0;
  std::string execution_job_id;
  std::string execution_system;
  std::optional<ExecutionResources> execution_resources;
  std::optional<RemoteExecutionState> execution_state;
  std::optional<uint64_t> batch_activation;
  std::optional<uint32_t> batch_index, batch_size, implementation_index;
  // A run list entry's label or an Input Card's swept values; "" for none.
  std::string implementation_label;
  // Card secret names handed to the Card's process; never their values.
  std::vector<std::string> secrets;
};

struct RunBatchOutputData {
  std::string port;
  uint32_t count = 0;
  bool execution_list = false;
};

struct RunBatchData {
  std::string run_id;
  CardPath card;
  uint64_t activation = 0;
  uint32_t invocation_count = 0;
  bool execution_list = false;
  bool complete = false;
  bool failed = false;
  std::vector<RunBatchOutputData> outputs;
};

struct SavedInputGroup {
  std::string port;
  bool execution_list = false;
  std::vector<Value> values;
  bool partial_result = false;
};

struct InputSourceData {
  std::string port;
  CardPath card;
  uint64_t invocation = 0;
};

struct ArtifactMemberData {
  std::string path;
  uint64_t size_bytes = 0;
  std::string hash;
};

struct ArtifactData {
  std::string artifact_id;
  std::string run_id;
  CardPath card;
  uint64_t invocation = 0;
  std::string output;
  PortKind kind = PortKind::File;
  std::string name;
  std::vector<ArtifactMemberData> members;
};

}  // namespace tsugumi
