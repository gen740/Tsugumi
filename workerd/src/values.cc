#include "tsugumi/workerd/values.hh"

#include <chrono>
#include <cstring>

#include "tsugumi/util/text.hh"

namespace tsugumi {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

const char* failure_kind_name(FailureKind k) {
  return Tsugumi::Data::EnumNameFailureKind(k);
}

std::optional<FailureKind> failure_kind_from_name(std::string_view n) {
  for (auto k : Tsugumi::Data::EnumValuesFailureKind())
    if (n == Tsugumi::Data::EnumNameFailureKind(k) &&
        k != FailureKind::unspecified)
      return k;
  return std::nullopt;
}

FailureData FailureData::make(
    FailureKind k, std::string diag,
    std::vector<std::pair<std::string, std::string>> args) {
  FailureData f;
  f.kind = k;
  if (!diag.empty()) f.diagnostic = text::replace_invalid_utf8(std::move(diag));
  f.args = std::move(args);
  return f;
}

}  // namespace tsugumi
