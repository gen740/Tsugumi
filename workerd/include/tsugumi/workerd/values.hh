// Worker invocation values, failures and opaque display envelopes.
#pragma once
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "tsugumi/document/definition_values.hh"
#include "tsugumi/document/types.hh"
namespace tsugumi {
using FailureKind = Tsugumi::Data::FailureKind;
using RunState = Tsugumi::Data::RunState;
using CardState = Tsugumi::Data::CardState;
using Phase = Tsugumi::Data::Phase;
using StageOutcome = Tsugumi::Data::StageOutcome;
using LogStream = Tsugumi::Data::LogStream;
using PortKind = Tsugumi::Data::PortKind;
using RemoteExecutionState = Tsugumi::Data::RemoteExecutionState;

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

// A value travelling on a port. `type` is the port message type.
struct Value {
  std::string name;  // port name
  PortKind kind = PortKind::FlatBuffer;
  types::TypeRef type;
  std::string bytes;
  std::optional<std::string> artifact_id;
};

auto with_path(const Value& value, const std::string& path) -> Value;
auto path_value(const Value& value) -> std::optional<std::string>;

// The core owns only the address and revision. Presentation remains an opaque
// verified FlatBuffer, including any shared tables and strings inside it.
struct DisplayData {
  CardPath card;
  std::string run_id;
  uint64_t invocation = 0;
  uint64_t revision = 0;
  std::string bytes;
  size_t root_offset = 0;
};

}  // namespace tsugumi
