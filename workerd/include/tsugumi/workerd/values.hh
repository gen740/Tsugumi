// Worker invocation values, failures and opaque display envelopes.
#pragma once
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tsugumi/document/definition_values.hh"
#include "tsugumi/document/types.hh"
#include "tsugumi/workerd/view.hh"
namespace tsugumi {
using FailureKind = Tsugumi::Data::FailureKind;
using RunState = Tsugumi::Data::RunState;
using CardState = Tsugumi::Data::CardState;
using Phase = Tsugumi::Data::Phase;
using StageOutcome = Tsugumi::Data::StageOutcome;
using LogStream = Tsugumi::Data::LogStream;
using RemoteExecutionState = Tsugumi::Data::RemoteExecutionState;

int64_t now_ms();

struct FailureData {
  FailureKind kind = FailureKind::internal_error;
  std::vector<std::pair<std::string, std::string>> args;
  std::optional<std::string> diagnostic;
  std::optional<uint64_t> current_rev;
  std::optional<uint32_t> retry_after_ms;

  static FailureData make(
      FailureKind k, std::string diag = {},
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
    return failure.diagnostic ? failure.diagnostic->c_str()
                              : failure_kind_name(failure.kind);
  }
  FailureData failure;
};

// Artifact ids in C++ are binary (exactly 16 bytes). Paths travel only
// in private grant messages.
using ArtifactKey = std::string;
struct ArtifactGrant {
  ArtifactKey id;
  std::string path;
  // Core-side holding layout used to stage the grant on a remote worker.
  std::filesystem::path location = {};
};

struct Value {
  std::string name;
  types::TypeRef type;
  std::string bytes;
  std::string schema;
  std::vector<ArtifactKey> artifacts = {};
};

}  // namespace tsugumi
