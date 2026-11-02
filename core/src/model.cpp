#include "model.hpp"

#include "schema.hpp"

#include <chrono>
#include <cmath>

#include "util/text.hpp"

namespace tsugumi {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

const char* failure_kind_name(FailureKind k) { return Tsugumi::Data::EnumNameFailureKind(k); }

std::optional<FailureKind> failure_kind_from_name(std::string_view n) {
  for (auto k : Tsugumi::Data::EnumValuesFailureKind())
    if (n == Tsugumi::Data::EnumNameFailureKind(k) && k != FailureKind::unspecified) return k;
  return std::nullopt;
}

FailureData FailureData::make(FailureKind k, std::string diag,
                              std::vector<std::pair<std::string, std::string>> args) {
  FailureData f;
  f.kind = k;
  if (!diag.empty()) f.diagnostic = std::move(diag);
  f.args = std::move(args);
  return f;
}

std::optional<std::string> DisplayData::validate() const {
  constexpr size_t kMaxPoints = 10000;
  constexpr size_t kMaxBytes = 1 << 20;
  if (const auto* t = std::get_if<Text>(&value)) {
    if (t->text.size() > kMaxBytes) return "display text exceeds 1 MiB";
    if (!text::valid_utf8(t->text)) return "display text is not UTF-8";
  } else if (const auto* s = std::get_if<Scalar>(&value)) {
    if (!std::isfinite(s->value)) return "display scalar must be finite";
  } else if (const auto* c = std::get_if<LineChart>(&value)) {
    size_t points = 0, bytes = 0;
    for (const auto& series : c->series) {
      if (series.x.size() != series.y.size()) return "series " + series.id + ": x and y differ in length";
      for (double v : series.x)
        if (!std::isfinite(v)) return "series " + series.id + ": non-finite x";
      for (double v : series.y)
        if (!std::isfinite(v)) return "series " + series.id + ": non-finite y";
      points += series.x.size();
      bytes += series.x.size() * 16 + series.id.size() + series.label.size();
    }
    if (points > kMaxPoints) return "line chart exceeds 10,000 points";
    if (bytes > kMaxBytes) return "line chart exceeds 1 MiB";
  } else if (const auto* t = std::get_if<Table>(&value)) {
    if (t->columns.empty() || t->columns.size() > 64) return "a table needs 1 to 64 columns";
    if (t->rows.size() > 10000) return "a table exceeds 10,000 rows";
    for (const auto& c : t->columns)
      if (!text::valid_utf8(c) || c.size() > 4096) return "table column names must be UTF-8 up to 4 KiB";
    for (const auto& r : t->rows) {
      if (r.size() != t->columns.size()) return "every table row needs one cell per column";
      for (const auto& c : r)
        if (c.size() > 4096 || !text::valid_utf8(c)) return "table cells must be UTF-8 up to 4 KiB";
    }
  } else if (const auto* im = std::get_if<Image>(&value)) {
    if (im->data.size() > (8u << 20)) return "an image exceeds 8 MiB";
    bool png = im->data.size() > 8 && im->data.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0;
    bool jpeg = im->data.size() > 3 && static_cast<unsigned char>(im->data[0]) == 0xFF &&
                static_cast<unsigned char>(im->data[1]) == 0xD8;
    if (im->media_type == "image/png" ? !png : im->media_type == "image/jpeg" ? !jpeg : true)
      return "images must be PNG or JPEG data matching media_type";
    if (!text::valid_utf8(im->alt)) return "image alt text is not UTF-8";
  } else if (const auto* p = std::get_if<PointCloud>(&value)) {
    if (p->x.size() != p->y.size() || p->x.size() != p->z.size()) return "point cloud x, y and z differ in length";
    if (p->x.size() > 1000000) return "a point cloud exceeds 1,000,000 points";
    if (!p->colors.empty() && p->colors.size() != p->x.size()) return "point colors must be empty or one per point";
    for (size_t i = 0; i < p->x.size(); ++i)
      if (!std::isfinite(p->x[i]) || !std::isfinite(p->y[i]) || !std::isfinite(p->z[i])) return "point coordinates must be finite";
  } else if (const auto* m = std::get_if<Module>(&value)) {
    if (!valid_id(m->module)) return "a Canvas module name must be an id";
    if (m->data_json.size() > kMaxBytes) return "Canvas module data exceeds 1 MiB";
    try {
      (void)schema::capture(m->data_json);
    } catch (const std::exception&) {
      return "Canvas module data is not valid JSON";
    }
  }
  return std::nullopt;
}

}  // namespace tsugumi
