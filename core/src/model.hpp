// Native data shared by the scheduler, the history store and RPC encoding.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "document.hpp"
#include "tsugumi/protocol/tsugumi_enums_generated.h"
#include "types.hpp"

namespace tsugumi {

using FailureKind = Tsugumi::Data::FailureKind;
using RunState = Tsugumi::Data::RunState;
using CardState = Tsugumi::Data::CardState;
using Phase = Tsugumi::Data::Phase;
using LogStream = Tsugumi::Data::LogStream;
using PortKind = Tsugumi::Data::PortKind;

int64_t now_ms();

struct FailureData {
  FailureKind kind = FailureKind::internal_error;
  std::vector<std::pair<std::string, std::string>> args;
  std::optional<std::string> diagnostic;
  std::optional<uint64_t> current_rev;
  std::optional<uint32_t> retry_after_ms;

  static FailureData make(FailureKind k, std::string diag = {},
                          std::vector<std::pair<std::string, std::string>> args = {});
  // Standalone Failure FlatBuffer (persistence format). Implemented in wire.cpp.
  std::string to_bytes() const;
  static std::optional<FailureData> from_bytes(const std::string& s);
};

const char* failure_kind_name(FailureKind k);
std::optional<FailureKind> failure_kind_from_name(std::string_view n);

// Exception carrying a structured failure to the RPC layer.
class Failed : public std::exception {
 public:
  explicit Failed(FailureData f) : failure(std::move(f)) {}
  Failed(FailureKind k, std::string diag,
         std::vector<std::pair<std::string, std::string>> args = {})
      : failure(FailureData::make(k, std::move(diag), std::move(args))) {}
  const char* what() const noexcept override {
    return failure.diagnostic ? failure.diagnostic->c_str() : failure_kind_name(failure.kind);
  }
  FailureData failure;
};

struct RunStatusData {
  std::optional<std::string> run_id;
  RunState state = RunState::idle;
  std::optional<uint64_t> rev, execution_rev;
  std::optional<int64_t> accepted_at, started_at, ended_at;
  std::optional<FailureData> failure;
  bool active() const {
    return state == RunState::queued || state == RunState::preparing ||
           state == RunState::running || state == RunState::cancelling;
  }
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

struct InvocationSummaryData {
  std::string run_id;
  CardPath card;
  uint64_t invocation = 0;
  CardState state = CardState::not_started;
  std::optional<Phase> phase;
  std::optional<int64_t> started_at, ended_at;
  std::optional<uint64_t> duration_ms;
  std::optional<FailureData> failure;
  std::vector<WaitingInputData> waiting;
};

// A value travelling on a port. `type` is the port message type.
struct Value {
  std::string name;  // port name
  PortKind kind = PortKind::FlatBuffer;
  types::TypeRef type;
  std::string bytes;
  std::optional<std::string> artifact_id;
};

struct InputSourceData {
  std::string port;
  CardPath card;
  uint64_t invocation = 0;
};

struct LineSeriesData {
  std::string id, label;
  std::vector<double> x, y;
};

struct DisplayData {
  CardPath card;
  std::string run_id;
  uint64_t invocation = 0;
  uint64_t revision = 0;
  struct Text { std::string text; };
  struct Scalar { double value = 0; std::string label; };
  struct LineChart { std::vector<LineSeriesData> series; };
  std::variant<Text, Scalar, LineChart> value;

  // Validates finiteness, x/y lengths and the 10,000 point / 1 MiB limits.
  std::optional<std::string> validate() const;
};

struct ArtifactMemberData {
  std::string path;
  uint64_t size_bytes = 0;
  std::string hash;
};

struct ArtifactData {
  std::string artifact_id, run_id;
  CardPath card;
  uint64_t invocation = 0;
  std::string output;
  PortKind kind = PortKind::File;
  std::string name;
  std::vector<ArtifactMemberData> members;
};

struct DiagnosticList {
  std::vector<Diag> items;
};

}  // namespace tsugumi
