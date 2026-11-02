#include "model.hpp"

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
  }
  return std::nullopt;
}

}  // namespace tsugumi
