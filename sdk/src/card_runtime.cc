// libtsugumi_card.so: the part of the C++ SDK that does not depend on a
// Card's contract (built once, shared by every C++ Card).
#include "tsugumi/card_runtime.hh"

namespace tsugumi {
namespace {

thread_local const tsugumi_call* g_call = nullptr;

const tsugumi_call& current() {
  if (!g_call) throw Failure("worker_failed", "no Card invocation is running");
  return *g_call;
}

void check(int rc, const char* what) {
  if (rc != 0) throw Failure("output_invalid", std::string(what) + " was rejected");
}

}  // namespace

auto Run::cancelled() const -> bool { return call_->cancelled(call_->host) != 0; }
auto Run::remaining_ms() const -> int64_t { return call_->remaining_ms(call_->host); }
auto Report::log(const std::string& message) const -> void { current().log(current().host, message.c_str()); }

auto Report::text(const std::string& text) const -> void {
  check(current().display_text(current().host, text.c_str()), "text display");
}
auto Report::markdown(const std::string& text) const -> void {
  check(current().display_markdown(current().host, text.c_str()), "markdown display");
}

auto Report::scalar(double value, const std::string& label) const -> void {
  check(current().display_scalar(current().host, value, label.c_str()), "scalar display");
}

auto Report::line_chart(const std::vector<Series>& series) const -> void {
  std::vector<tsugumi_series> s;
  for (const auto& x : series) {
    if (x.x.size() != x.y.size()) throw Failure("output_invalid", "series x and y differ in length");
    s.push_back({x.id.c_str(), x.label.c_str(), x.x.data(), x.y.data(), x.x.size()});
  }
  check(current().display_line_chart(current().host, s.data(), s.size()), "line chart");
}

auto Report::table(const std::vector<std::string>& columns, const std::vector<std::vector<std::string>>& rows) const
    -> void {
  std::vector<const char*> cols;
  std::vector<const char*> cells;
  for (const auto& c : columns) cols.push_back(c.c_str());
  for (const auto& r : rows) {
    if (r.size() != columns.size()) throw Failure("output_invalid", "table rows need one cell per column");
    for (const auto& c : r) cells.push_back(c.c_str());
  }
  check(current().display_table(current().host, cols.data(), cols.size(), cells.data(), rows.size()), "table");
}

auto Report::image(const std::string& media_type, const std::string& bytes, const std::string& alt) const -> void {
  check(current().display_image(current().host, media_type.c_str(), reinterpret_cast<const uint8_t*>(bytes.data()),
                                bytes.size(), alt.c_str()),
        "image");
}

auto Report::points(const std::vector<float>& x, const std::vector<float>& y, const std::vector<float>& z,
                    const std::vector<uint32_t>& colors) const -> void {
  if (x.size() != y.size() || x.size() != z.size()) throw Failure("output_invalid", "x, y and z differ in length");
  check(current().display_points(current().host, x.data(), y.data(), z.data(), colors.empty() ? nullptr : colors.data(),
                                 x.size()),
        "point cloud");
}

auto Report::module(const std::string& name, const std::string& config_json,
                    const std::vector<tsugumi_blob>& blobs) const -> void {
  check(current().display_module(current().host, name.c_str(), config_json.c_str(), blobs.data(), blobs.size()),
        "module display");
}

namespace card {

const uint8_t* Call::input(std::string_view port) const {
  for (size_t i = 0; i < c_->input_count; ++i)
    if (port == c_->inputs[i].name) return c_->inputs[i].data;
  return nullptr;
}

auto Call::finish_outputs() const -> void { check(c_->finish_outputs(c_->host), "required outputs"); }

void Call::emit(std::string_view port, const flatbuffers::FlatBufferBuilder& b) const {
  std::string p(port);
  if (c_->emit_output(c_->host, p.c_str(), b.GetBufferPointer(), b.GetSize()) != 0)
    throw Failure("output_invalid", "output " + p + " was rejected");
}

int invoke(const tsugumi_call* call, const std::function<void(Call&)>& body) {
  if (call->abi_version != TSUGUMI_ABI_VERSION) {
    call->fail(call->host, "worker_failed", "ABI version mismatch");
    return 1;
  }
  g_call = call;
  struct Reset {
    ~Reset() { g_call = nullptr; }
  } reset;
  try {
    Call c(call);
    body(c);
    return 0;
  } catch (const Failure& f) {
    call->fail(call->host, f.kind.c_str(), f.what());
  } catch (const std::exception& e) {
    call->fail(call->host, "worker_failed", e.what());
  } catch (...) {
    call->fail(call->host, "worker_failed", "unknown C++ exception");
  }
  return 1;
}

}  // namespace card
}  // namespace tsugumi
