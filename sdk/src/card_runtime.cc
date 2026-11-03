// libtsugumi_card.so: the part of the C++ SDK that does not depend on a
// Card's contract (built once, shared by every C++ Card).
#include "tsugumi/card_runtime.hh"

namespace tsugumi {
namespace {

thread_local const tsugumi_call* g_call = nullptr;
thread_local Context g_context;

const tsugumi_call& current() {
  if (!g_call) throw Failure("worker_failed", "no Card invocation is running");
  return *g_call;
}

void check(int rc, const char* what) {
  if (rc != 0) throw Failure("output_invalid", std::string(what) + " was rejected");
}

}  // namespace

const Context& context() {
  current();
  return g_context;
}
bool cancelled() { return current().cancelled(current().host) != 0; }
int64_t remaining_ms() { return current().remaining_ms(current().host); }
void log(const std::string& message) { current().log(current().host, message.c_str()); }

namespace visualization {

void text(const std::string& text) { check(current().display_text(current().host, text.c_str()), "text display"); }
void markdown(const std::string& text) {
  check(current().display_markdown(current().host, text.c_str()), "markdown display");
}

void scalar(double value, const std::string& label) {
  check(current().display_scalar(current().host, value, label.c_str()), "scalar display");
}

void line_chart(const std::vector<Series>& series) {
  std::vector<tsugumi_series> s;
  for (const auto& x : series) {
    if (x.x.size() != x.y.size()) throw Failure("output_invalid", "series x and y differ in length");
    s.push_back({x.id.c_str(), x.label.c_str(), x.x.data(), x.y.data(), x.x.size()});
  }
  check(current().display_line_chart(current().host, s.data(), s.size()), "line chart");
}

void table(const std::vector<std::string>& columns, const std::vector<std::vector<std::string>>& rows) {
  std::vector<const char*> cols, cells;
  for (const auto& c : columns) cols.push_back(c.c_str());
  for (const auto& r : rows) {
    if (r.size() != columns.size()) throw Failure("output_invalid", "table rows need one cell per column");
    for (const auto& c : r) cells.push_back(c.c_str());
  }
  check(current().display_table(current().host, cols.data(), cols.size(), cells.data(), rows.size()), "table");
}

void image(const std::string& media_type, const std::string& bytes, const std::string& alt) {
  check(current().display_image(current().host, media_type.c_str(), reinterpret_cast<const uint8_t*>(bytes.data()),
                                bytes.size(), alt.c_str()),
        "image");
}

void points(const std::vector<float>& x, const std::vector<float>& y, const std::vector<float>& z,
            const std::vector<uint32_t>& colors) {
  if (x.size() != y.size() || x.size() != z.size()) throw Failure("output_invalid", "x, y and z differ in length");
  check(current().display_points(current().host, x.data(), y.data(), z.data(), colors.empty() ? nullptr : colors.data(),
                                 x.size()),
        "point cloud");
}

void module(const std::string& name, const std::string& data_json) {
  check(current().display_module(current().host, name.c_str(), data_json.c_str()), "module display");
}

}  // namespace visualization

namespace card {

const uint8_t* Call::input(std::string_view port) const {
  for (size_t i = 0; i < c_->input_count; ++i)
    if (port == c_->inputs[i].name) return c_->inputs[i].data;
  return nullptr;
}

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
  g_context = Context{call->run_id, call->card, call->invocation, call->working_directory, call->temporary_directory,
                      call->output_directory};
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
