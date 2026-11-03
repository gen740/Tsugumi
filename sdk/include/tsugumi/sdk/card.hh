// C++ convenience layer over the Tsugumi Card ABI.
//
//   #include <tsugumi/sdk/card.hh>
//   #include "_inputs_generated.h"
//   #include "_outputs_generated.h"
//
//   void run(tsugumi::sdk::Context& ctx) {
//     const auto* n = ctx.input<inputs::value::collatz::Number>("value");
//     flatbuffers::FlatBufferBuilder b;
//     b.Finish(outputs::next::collatz::CreateNumber(b, n->value() / 2));
//     ctx.output("next", b);
//   }
//   TSUGUMI_CARD(run)
#pragma once

#include <flatbuffers/flatbuffers.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "tsugumi/sdk/abi.h"

namespace tsugumi::sdk {

// Throw to fail the invocation with a structured FailureKind.
struct Failure : std::runtime_error {
  Failure(std::string kind, const std::string& message) : std::runtime_error(message), kind(std::move(kind)) {}
  std::string kind;
};

struct Series {
  std::string id, label;
  std::vector<double> x, y;
};

class Context {
 public:
  explicit Context(const tsugumi_call* call) : c_(call) {
    if (call->abi_version != TSUGUMI_ABI_VERSION) throw Failure("worker_failed", "ABI version mismatch");
  }

  std::string_view run_id() const { return c_->run_id; }
  std::string_view card() const { return c_->card; }
  uint64_t invocation() const { return c_->invocation; }
  std::string working_directory() const { return c_->working_directory; }
  std::string temporary_directory() const { return c_->temporary_directory; }
  std::string output_directory() const { return c_->output_directory; }

  bool has_input(std::string_view port) const { return find(port) != nullptr; }

  // Returns the input message; throws input_missing when absent. Buffers are
  // verified by the core before the call.
  template <class T>
  const T* input(std::string_view port) const {
    const tsugumi_value* v = find(port);
    if (!v) throw Failure("input_missing", "input " + std::string(port) + " was not provided");
    return flatbuffers::GetRoot<T>(v->data);
  }

  template <class T>
  const T* params() const {
    return flatbuffers::GetRoot<T>(c_->params);
  }

  void output(std::string_view port, const flatbuffers::FlatBufferBuilder& b) const {
    std::string p(port);
    if (c_->emit_output(c_->host, p.c_str(), b.GetBufferPointer(), b.GetSize()) != 0)
      throw Failure("output_invalid", "output " + p + " was rejected");
  }

  void display_text(const std::string& text) const { c_->display_text(c_->host, text.c_str()); }
  void display_markdown(const std::string& text) const { c_->display_markdown(c_->host, text.c_str()); }
  void display_scalar(double value, const std::string& label) const {
    c_->display_scalar(c_->host, value, label.c_str());
  }
  void display_line_chart(const std::vector<Series>& series) const {
    std::vector<tsugumi_series> s;
    for (const auto& x : series) {
      if (x.x.size() != x.y.size()) throw Failure("output_invalid", "series x and y differ in length");
      s.push_back({x.id.c_str(), x.label.c_str(), x.x.data(), x.y.data(), x.x.size()});
    }
    c_->display_line_chart(c_->host, s.data(), s.size());
  }

  void display_table(const std::vector<std::string>& columns, const std::vector<std::vector<std::string>>& rows) const {
    std::vector<const char*> cols, cells;
    for (const auto& c : columns) cols.push_back(c.c_str());
    for (const auto& r : rows) {
      if (r.size() != columns.size()) throw Failure("output_invalid", "table rows need one cell per column");
      for (const auto& c : r) cells.push_back(c.c_str());
    }
    c_->display_table(c_->host, cols.data(), cols.size(), cells.data(), rows.size());
  }
  void display_image(const std::string& media_type, const std::string& bytes, const std::string& alt = "") const {
    c_->display_image(c_->host, media_type.c_str(), reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                      alt.c_str());
  }
  void display_points(const std::vector<float>& x, const std::vector<float>& y, const std::vector<float>& z,
                      const std::vector<uint32_t>& colors = {}) const {
    if (x.size() != y.size() || x.size() != z.size()) throw Failure("output_invalid", "x, y and z differ in length");
    c_->display_points(c_->host, x.data(), y.data(), z.data(), colors.empty() ? nullptr : colors.data(), x.size());
  }
  void display_module(const std::string& name, const std::string& data_json) const {
    c_->display_module(c_->host, name.c_str(), data_json.c_str());
  }

  void log(const std::string& message) const { c_->log(c_->host, message.c_str()); }
  bool cancelled() const { return c_->cancelled(c_->host) != 0; }
  int64_t remaining_ms() const { return c_->remaining_ms(c_->host); }

 private:
  const tsugumi_value* find(std::string_view port) const {
    for (size_t i = 0; i < c_->input_count; ++i)
      if (port == c_->inputs[i].name) return &c_->inputs[i];
    return nullptr;
  }
  const tsugumi_call* c_;
};

}  // namespace tsugumi::sdk

#define TSUGUMI_CARD(fn)                                                  \
  extern "C" int tsugumi_main(const tsugumi_call* call) {                 \
    try {                                                                 \
      tsugumi::sdk::Context ctx(call);                                    \
      fn(ctx);                                                            \
      return 0;                                                           \
    } catch (const tsugumi::sdk::Failure& f) {                            \
      call->fail(call->host, f.kind.c_str(), f.what());                   \
    } catch (const std::exception& e) {                                   \
      call->fail(call->host, "worker_failed", e.what());                  \
    } catch (...) {                                                       \
      call->fail(call->host, "worker_failed", "unknown C++ exception");   \
    }                                                                     \
    return 1;                                                             \
  }
