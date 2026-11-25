// Worker invocation values, failures and opaque display envelopes.
#pragma once
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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

// Artifact keys are the 16 bytes of a UUID.
using ArtifactKey = std::string;
struct CreatedArtifact {
  ArtifactKey id;
  bool keep;
};

namespace workerd {
class SharedBuffer;
}

class PayloadBytes {
 public:
  PayloadBytes() = default;
  PayloadBytes(std::string bytes) : storage_(std::move(bytes)) {}
  PayloadBytes(std::string_view bytes) : PayloadBytes(std::string(bytes)) {}
  PayloadBytes(const char* bytes) : PayloadBytes(std::string_view(bytes)) {}
  PayloadBytes(const char* bytes, size_t size)
      : PayloadBytes(std::string(bytes, size)) {}

  static auto borrow(std::string_view bytes, std::shared_ptr<const void> owner)
      -> PayloadBytes {
    return PayloadBytes(Borrowed{bytes, std::move(owner)});
  }

  static auto shared(std::string_view bytes,
                     std::shared_ptr<const workerd::SharedBuffer> buffer)
      -> PayloadBytes {
    return PayloadBytes(Borrowed{bytes, std::move(buffer)});
  }

  auto shared_buffer() const -> std::shared_ptr<const workerd::SharedBuffer> {
    if (const auto* borrowed = std::get_if<Borrowed>(&storage_))
      if (const auto* buffer =
              std::get_if<std::shared_ptr<const workerd::SharedBuffer>>(
                  &borrowed->owner))
        return *buffer;
    return {};
  }

  auto view() const -> std::string_view {
    if (const auto* borrowed = std::get_if<Borrowed>(&storage_))
      return borrowed->bytes;
    return std::get<std::string>(storage_);
  }

  operator std::string_view() const { return view(); }
  auto data() const -> const char* { return view().data(); }
  auto size() const -> size_t { return view().size(); }
  auto empty() const -> bool { return view().empty(); }
  auto begin() const { return view().begin(); }
  auto end() const { return view().end(); }

  template <typename It>
  auto assign(It first, It last) -> void {
    storage_ = std::string(first, last);
  }

  friend auto operator==(const PayloadBytes& lhs, const PayloadBytes& rhs)
      -> bool {
    return lhs.view() == rhs.view();
  }

 private:
  struct Borrowed {
    std::string_view bytes;
    std::variant<std::shared_ptr<const void>,
                 std::shared_ptr<const workerd::SharedBuffer>> owner;
  };

  explicit PayloadBytes(Borrowed borrowed) : storage_(std::move(borrowed)) {}
  std::variant<std::string, Borrowed> storage_;
};

struct Value {
  std::string name;
  types::TypeRef type;
  PayloadBytes bytes;
  std::string schema;
};

}  // namespace tsugumi
