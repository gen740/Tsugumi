#include "model.hh"

#include <chrono>
#include <cstring>
#include <flatbuffers/reflection.h>


namespace tsugumi {

// Returns a copy of a File/Directory value whose `value` field is `p`.
auto with_path(const Value& v, const std::string& p) -> Value {
  Value out = v;
  std::vector<uint8_t> buf(v.bytes.begin(), v.bytes.end());
  const auto* root = v.type->root();
  const auto* f = root->fields()->LookupByKey("value");
  auto* t = flatbuffers::GetAnyRoot(buf.data());
  const auto* s = flatbuffers::GetFieldS(*t, *f);
  if (!s) throw Failed(FailureKind::output_invalid, "file value " + v.name + " has no path");
  if (p.size() <= s->size()) {
    // Shrinking in place: flatbuffers' ResizeContext erases the bytes before
    // the string when it shrinks, which corrupts the buffer. A shorter string
    // with trailing padding is still a valid buffer.
    auto* at = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(s));
    flatbuffers::WriteScalar<flatbuffers::uoffset_t>(at, static_cast<flatbuffers::uoffset_t>(p.size()));
    std::memcpy(at + sizeof(flatbuffers::uoffset_t), p.c_str(), p.size() + 1);
  } else {
    flatbuffers::SetString(*v.type->schema(), p, s, &buf, root);
  }
  out.bytes.assign(reinterpret_cast<const char*>(buf.data()), buf.size());
  return out;
}

// Reads the string `value` field of a File/Directory message.
auto path_value(const Value& v) -> std::optional<std::string> {
  const auto* root = v.type->root();
  const auto* f = root->fields()->LookupByKey("value");
  if (!f) return std::nullopt;
  const auto* t = flatbuffers::GetAnyRoot(reinterpret_cast<const uint8_t*>(v.bytes.data()));
  const auto* s = flatbuffers::GetFieldS(*t, *f);
  if (!s) return std::nullopt;
  return s->str();
}

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

}  // namespace tsugumi
