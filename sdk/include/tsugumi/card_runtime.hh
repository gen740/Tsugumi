// The Tsugumi Card runtime (libtsugumi_card.so): one shared library for every
// C++ Card, built once per environment and reused by all SDKs. Ordinary Cards
// only include the generated <card.hh> and write
//
//   #include <card.hh>
//
//   auto run(const Card::Inputs& inputs, tsugumi::Report& report) ->
//   Card::Output {
//     Card::Output out;
//     out.next.emplace().value = inputs.number.value / 2;
//     report.text("halved");
//     return out;
//   }
//
// Everything below the `card` namespace is for generated adapters.
#pragma once

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "tsugumi/sdk/abi.h"

namespace tsugumi {

// Throw to fail the invocation with a FailureKind name ("output_invalid", ...).
struct Failure : std::runtime_error {
  Failure(std::string kind, const std::string& message)
      : std::runtime_error(message), kind(std::move(kind)) {}
  std::string kind;
};

// Invocation metadata is supplied only to Cards that request the third role.
struct Run {
  explicit Run(const tsugumi_call* call)
      : run_id(call->run_id),
        card(call->card),
        invocation(call->invocation),
        working_directory(call->working_directory),
        temporary_directory(call->temporary_directory),
        output_directory(call->output_directory),
        call_(call) {}
  std::string run_id;
  std::string card;
  uint64_t invocation;
  std::string working_directory;
  std::string temporary_directory;
  std::string output_directory;
  auto cancelled() const -> bool;
  auto remaining_ms() const -> int64_t;

 private:
  const tsugumi_call* call_;
};

struct Report {
  auto log(const std::string& message) const -> void;
  struct Series {
    std::string id;
    std::string label;
    std::vector<double> x;
    std::vector<double> y;
  };
  auto text(const std::string& text) const -> void;
  /// Markdown with TeX math ($…$ inline, $$…$$ displayed), shown formatted.
  auto markdown(const std::string& text) const -> void;
  auto scalar(double value, const std::string& label) const -> void;
  auto line_chart(const std::vector<Series>& series) const -> void;
  auto table(const std::vector<std::string>& columns,
             const std::vector<std::vector<std::string>>& rows) const -> void;
  auto image(const std::string& media_type, const std::string& bytes,
             const std::string& alt = "") const -> void;
  auto points(const std::vector<float>& x, const std::vector<float>& y,
              const std::vector<float>& z,
              const std::vector<uint32_t>& colors = {}) const -> void;
  auto module(const std::string& module, const std::string& config_json,
              const std::vector<tsugumi_blob>& blobs = {}) const -> void;
};

namespace card {

// One call as seen by a generated adapter.
class Call {
 public:
  explicit Call(const tsugumi_call* call) : c_(call) {}

  template <class Table>
  auto input(const char* port) const -> const Table* {
    size_t size;
    const auto* bytes = c_->effects->input(c_->host, port, &size);
    return bytes ? flatbuffers::GetRoot<Table>(bytes) : nullptr;
  }

  auto output(const char* port,
              const flatbuffers::FlatBufferBuilder& buffer) const -> void {
    if (c_->effects->output(c_->host, port, buffer.GetBufferPointer(),
                            buffer.GetSize()))
      throw Failure("output_invalid", port);
  }

  template <class Table, class T>
  void unpack_params(T& out) const {
    if (c_->params && c_->params_size)
      flatbuffers::GetRoot<Table>(c_->params)->UnPackTo(&out);
  }
 private:
  const tsugumi_call* c_;
};

// Runs `body` as the invocation of `call`: sets the active run, turns
// exceptions into failures and returns tsugumi_main's result.
int invoke(const tsugumi_call* call, const std::function<void(Call&)>& body);

}  // namespace card
}  // namespace tsugumi
