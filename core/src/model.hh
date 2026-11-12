// Native data shared by the scheduler, the history store and RPC encoding.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "document.hh"
#include "tsugumi/protocol/tsugumi_enums_generated.h"
#include "types.hh"

namespace tsugumi {

using FailureKind = Tsugumi::Data::FailureKind;
using RunState = Tsugumi::Data::RunState;
using CardState = Tsugumi::Data::CardState;
using Phase = Tsugumi::Data::Phase;
using StageOutcome = Tsugumi::Data::StageOutcome;
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
  // Standalone Failure FlatBuffer (persistence format). Implemented in wire.cc.
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
  std::optional<uint64_t> rev;
  std::optional<uint64_t> execution_rev;
  std::optional<int64_t> accepted_at;
  std::optional<int64_t> started_at;
  std::optional<int64_t> ended_at;
  std::optional<FailureData> failure;
  std::optional<std::string> commit;  // the project's version the run started from
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

struct StageTimeData {
  Phase phase = Phase::unspecified;
  StageOutcome outcome = StageOutcome::running;
  int64_t start = 0;
  std::optional<int64_t> end;
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
  std::vector<StageTimeData> stages;
  std::optional<bool> worker_reused;
  std::vector<std::string> outputs_sent;
  std::optional<int64_t> ready_at;
  // The program the kernel ran the Card's binaries with, for a Card built for
  // another system (binfmt_misc); "" when it ran natively.
  std::string emulator;
  std::string environment_id;
  std::string params_json;  // the invocation's parameters, not the current document
  std::string execution_service;
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
  std::string id;
  std::string label;
  std::vector<double> x;
  std::vector<double> y;
};

// The kinds of display, outside DisplayData: clang with libstdc++ does not
// count a nested struct with member initializers as default-constructible
// inside its enclosing class, so the variant would have no default.
namespace display {
struct Text {
  std::string text;
  bool markdown = false;
};
struct Scalar { double value = 0; std::string label; };
struct LineChart { std::vector<LineSeriesData> series; };
struct Table { std::vector<std::string> columns; std::vector<std::vector<std::string>> rows; };
struct Image { std::string media_type; std::string data; std::string alt; };
struct PointCloud {
  std::vector<float> x;
  std::vector<float> y;
  std::vector<float> z;
  std::vector<uint32_t> colors;
};
struct Module { std::string module; std::string data_json; };
}  // namespace display

struct DisplayData {
  CardPath card;
  std::string run_id;
  uint64_t invocation = 0;
  uint64_t revision = 0;
  using Text = display::Text;
  using Scalar = display::Scalar;
  using LineChart = display::LineChart;
  using Table = display::Table;
  using Image = display::Image;
  using PointCloud = display::PointCloud;
  using Module = display::Module;
  std::variant<Text, Scalar, LineChart, Table, Image, PointCloud, Module> value;

  // Validates structure and size limits of every variant (see data.fbs).
  std::optional<std::string> validate() const;
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

struct DiagnosticList {
  std::vector<Diag> items;
};

}  // namespace tsugumi
