#include "model.hh"

#include "schema.hh"

#include <chrono>
#include <cmath>
#include <cstring>
#include <flatbuffers/reflection.h>

#include "util/text.hh"

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

auto valid_module_identity(std::string_view identity) -> bool {
  if (!identity.starts_with("/nix/store/") || identity.size() < 45 || identity[43] != '-') return false;
  constexpr std::string_view hash = "0123456789abcdfghijklmnpqrsvwxyz";
  for (size_t i = 11; i < 43; ++i)
    if (hash.find(identity[i]) == std::string_view::npos) return false;
  constexpr std::string_view name = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-._?=";
  return identity.substr(44).find_first_not_of(name) == std::string_view::npos;
}

std::optional<std::string> DisplayData::validate() const {
  constexpr size_t kMaxBytes = 1 << 20;
  if (const auto* t = std::get_if<Text>(&value)) {
    if (t->text.size() > kMaxBytes) return "display text exceeds 1 MiB";
    if (!text::valid_utf8(t->text)) return "display text is not UTF-8";
  } else if (const auto* s = std::get_if<Scalar>(&value)) {
    if (!std::isfinite(s->value)) return "display scalar must be finite";
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
    if (!valid_module_identity(m->module)) return "a module must name a Nix store directory";
    if (m->blobs.size() > 64) return "a module exceeds 64 blobs";
    size_t bytes = 0;
    for (const auto& [name, data] : m->blobs) {
      if (!valid_id(name)) return "a module blob name must be an id";
      bytes += data.size();
      if (bytes > (8u << 20)) return "module blobs exceed 8 MiB";
    }
    if (m->config_json.size() > kMaxBytes) return "module config exceeds 1 MiB";
    try {
      (void)schema::capture(m->config_json);
    } catch (const std::exception&) {
      return "module config is not valid JSON";
    }
  }
  return std::nullopt;
}

}  // namespace tsugumi
