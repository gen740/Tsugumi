// The Tsugumi Card runtime (libtsugumi_card.so): one shared library for every
// C++ Card, built once per environment and reused by all SDKs. Ordinary Cards
// only include the generated <card.hh> and write
//
//   #include <card.hh>
//
//   auto run(Card::Input& input) -> Card::Output {
//     Card::Output out;
//     out.next.emplace().value = input.number.value / 2;
//     tsugumi::visualization::text("halved");
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
#include <string_view>
#include <vector>

#include "tsugumi/sdk/abi.h"

namespace tsugumi {

// Throw to fail the invocation with a FailureKind name ("output_invalid", ...).
struct Failure : std::runtime_error {
  Failure(std::string kind, const std::string& message) : std::runtime_error(message), kind(std::move(kind)) {}
  std::string kind;
};

// The running invocation, for the few Cards that need more than their inputs.
struct Context {
  std::string run_id;
  std::string card;  // full Card path joined with '/'
  uint64_t invocation = 0;
  std::string working_directory;
  std::string temporary_directory;
  std::string output_directory;
};
const Context& context();
bool cancelled();
int64_t remaining_ms();  // -1 without a deadline
void log(const std::string& message);  // a line in the invocation's stderr log

namespace visualization {
struct Series {
  std::string id;
  std::string label;
  std::vector<double> x;
  std::vector<double> y;
};
void text(const std::string& text);
/// Markdown with TeX math ($…$ inline, $$…$$ displayed), shown formatted.
void markdown(const std::string& text);
void scalar(double value, const std::string& label);
void line_chart(const std::vector<Series>& series);
void table(const std::vector<std::string>& columns, const std::vector<std::vector<std::string>>& rows);
void image(const std::string& media_type, const std::string& bytes, const std::string& alt = "");
void points(const std::vector<float>& x, const std::vector<float>& y, const std::vector<float>& z,
            const std::vector<uint32_t>& colors = {});
auto module(const std::string& module, const std::string& config_json,
            const std::vector<tsugumi_blob>& blobs = {}) -> void;
}  // namespace visualization

namespace card {

// One call as seen by a generated adapter.
class Call {
 public:
  explicit Call(const tsugumi_call* call) : c_(call) {}

  const uint8_t* input(std::string_view port) const;  // nullptr when absent
  const uint8_t* params() const { return c_->params; }
  void emit(std::string_view port, const flatbuffers::FlatBufferBuilder& b) const;

  template <class Table, class T>
  void unpack(std::string_view port, T& out) const {
    const uint8_t* d = input(port);
    if (!d) throw Failure("input_missing", "input " + std::string(port) + " was not provided");
    flatbuffers::GetRoot<Table>(d)->UnPackTo(&out);
  }
  template <class Table, class T>
  void unpack(std::string_view port, std::optional<T>& out) const {
    if (const uint8_t* d = input(port)) flatbuffers::GetRoot<Table>(d)->UnPackTo(&out.emplace());
  }
  template <class Table, class T>
  void unpack_params(T& out) const {
    if (c_->params && c_->params_size) flatbuffers::GetRoot<Table>(c_->params)->UnPackTo(&out);
  }
  template <class Table, class T>
  void emit_object(std::string_view port, const T& obj) const {
    flatbuffers::FlatBufferBuilder b;
    b.Finish(Table::Pack(b, &obj));
    emit(port, b);
  }

 private:
  const tsugumi_call* c_;
};

// Runs `body` as the invocation of `call`: sets the context, turns exceptions
// into failures and returns tsugumi_main's result.
int invoke(const tsugumi_call* call, const std::function<void(Call&)>& body);

}  // namespace card
}  // namespace tsugumi
