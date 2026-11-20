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
  if (rc != 0)
    throw Failure("output_invalid", std::string(what) + " was rejected");
}

}  // namespace

auto Run::cancelled() const -> bool {
  return call_->effects->cancelled(call_->host) != 0;
}
auto Run::remaining_ms() const -> int64_t {
  return call_->effects->remaining_ms(call_->host);
}
auto Report::log(const std::string& message) const -> void {
  current().effects->log(current().host, message.c_str());
}

auto Report::text(const std::string& text) const -> void {
  check(tsugumi_display_text(&current(), text.c_str(), 0), "text display");
}
auto Report::markdown(const std::string& text) const -> void {
  check(tsugumi_display_text(&current(), text.c_str(), 1), "markdown display");
}

auto Report::scalar(double value, const std::string& label) const -> void {
  check(tsugumi_display_scalar(&current(), value, label.c_str()),
        "scalar display");
}

auto Report::line_chart(const std::vector<Series>& series) const -> void {
  std::vector<tsugumi_series> s;
  for (const auto& x : series) {
    if (x.x.size() != x.y.size())
      throw Failure("output_invalid", "series x and y differ in length");
    s.push_back(
        {x.id.c_str(), x.label.c_str(), x.x.data(), x.y.data(), x.x.size()});
  }
  check(tsugumi_display_chart(&current(), s.data(), s.size()), "line chart");
}

auto Report::table(const std::vector<std::string>& columns,
                   const std::vector<std::vector<std::string>>& rows) const
    -> void {
  std::vector<const char*> cols;
  std::vector<const char*> cells;
  for (const auto& c : columns) cols.push_back(c.c_str());
  for (const auto& r : rows) {
    if (r.size() != columns.size())
      throw Failure("output_invalid", "table rows need one cell per column");
    for (const auto& c : r) cells.push_back(c.c_str());
  }
  check(tsugumi_display_table(&current(), cols.data(), cols.size(),
                              cells.data(), rows.size()),
        "table");
}

auto Report::image(const std::string& media_type, const std::string& bytes,
                   const std::string& alt) const -> void {
  check(tsugumi_display_image(&current(), media_type.c_str(),
                              reinterpret_cast<const uint8_t*>(bytes.data()),
                              bytes.size(), alt.c_str()),
        "image");
}

auto Report::points(const std::vector<float>& x, const std::vector<float>& y,
                    const std::vector<float>& z,
                    const std::vector<uint32_t>& colors) const -> void {
  if (x.size() != y.size() || x.size() != z.size())
    throw Failure("output_invalid", "x, y and z differ in length");
  if (!colors.empty() && colors.size() != x.size())
    throw Failure("output_invalid", "colors must be empty or match x length");
  check(tsugumi_display_points(&current(), x.data(), y.data(), z.data(),
                               colors.empty() ? nullptr : colors.data(),
                               x.size()),
        "point cloud");
}

auto Report::module(const std::string& name, const std::string& config_json,
                    const std::vector<tsugumi_blob>& blobs) const -> void {
  check(tsugumi_display_module(&current(), name.c_str(), config_json.c_str(),
                               blobs.data(), blobs.size()),
        "module display");
}

namespace card {

int invoke(const tsugumi_call* call, const std::function<void(Call&)>& body) {
  g_call = call;
  struct Reset {
    ~Reset() { g_call = nullptr; }
  } reset;
  try {
    Call c(call);
    body(c);
    return 0;
  } catch (const Failure& f) {
    call->effects->fail(call->host, f.kind.c_str(), f.what());
  } catch (const std::exception& e) {
    call->effects->fail(call->host, "worker_failed", e.what());
  } catch (...) {
    call->effects->fail(call->host, "worker_failed", "unknown C++ exception");
  }
  return 1;
}

}  // namespace card
}  // namespace tsugumi
