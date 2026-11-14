// Presentation validation belongs to the common Card ABI, inside the worker.
#pragma once

#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "tsugumi/protocol/data_generated.h"
#include "schema.hh"
#include "util/text.hh"

namespace tsugumi::workerd {
inline auto validate_display(const Tsugumi::Data::Display& display) -> std::optional<std::string> {
  namespace D = Tsugumi::Data;
  const auto utf8 = [](const flatbuffers::String* s) { return s && text::valid_utf8(s->string_view()); };
  constexpr size_t kMaxBytes = 1u << 20;
  switch (display.value_type()) {
    case D::DisplayValue::DisplayText: {
      const auto* t = display.value_as_DisplayText();
      if (!t || !t->text() || t->text()->size() > kMaxBytes) return "display text exceeds 1 MiB";
      if (!utf8(t->text())) return "display text is not UTF-8";
      break;
    }
    case D::DisplayValue::DisplayScalar: {
      const auto* s = display.value_as_DisplayScalar();
      if (!s || !s->value() || !std::isfinite(*s->value())) return "display scalar must be finite";
      break;
    }
    case D::DisplayValue::DisplayTable: {
      const auto* t = display.value_as_DisplayTable();
      if (!t || !t->columns() || t->columns()->size() == 0 || t->columns()->size() > 64)
        return "a table needs 1 to 64 columns";
      if (!t->rows() || t->rows()->size() > 10000) return "a table exceeds 10,000 rows";
      for (const auto* c : *t->columns())
        if (!utf8(c) || c->size() > 4096) return "table column names must be UTF-8 up to 4 KiB";
      for (const auto* r : *t->rows()) {
        if (!r || !r->cells() || r->cells()->size() != t->columns()->size())
          return "every table row needs one cell per column";
        for (const auto* c : *r->cells())
          if (!utf8(c) || c->size() > 4096) return "table cells must be UTF-8 up to 4 KiB";
      }
      break;
    }
    case D::DisplayValue::DisplayImage: {
      const auto* im = display.value_as_DisplayImage();
      if (!im || !im->data() || im->data()->size() > (8u << 20)) return "an image exceeds 8 MiB";
      const std::string_view bytes(reinterpret_cast<const char*>(im->data()->data()), im->data()->size());
      const auto media = im->media_type() ? im->media_type()->string_view() : "";
      const bool png = bytes.size() > 8 && bytes.starts_with("\x89PNG\r\n\x1a\n");
      const bool jpeg = bytes.size() > 3 && bytes.starts_with("\xff\xd8");
      if (media == "image/png" ? !png : media == "image/jpeg" ? !jpeg : true)
        return "images must be PNG or JPEG data matching media_type";
      if (im->alt() && !utf8(im->alt())) return "image alt text is not UTF-8";
      break;
    }
    case D::DisplayValue::DisplayPointCloud: {
      const auto* p = display.value_as_DisplayPointCloud();
      if (!p || !p->x() || !p->y() || !p->z() || p->x()->size() != p->y()->size() || p->x()->size() != p->z()->size())
        return "point cloud x, y and z differ in length";
      if (p->x()->size() > 1000000) return "a point cloud exceeds 1,000,000 points";
      if (p->colors() && p->colors()->size() && p->colors()->size() != p->x()->size())
        return "point colors must be empty or one per point";
      for (size_t i = 0; i < p->x()->size(); ++i)
        if (!std::isfinite(p->x()->Get(i)) || !std::isfinite(p->y()->Get(i)) || !std::isfinite(p->z()->Get(i)))
          return "point coordinates must be finite";
      break;
    }
    case D::DisplayValue::DisplayModule: {
      const auto* m = display.value_as_DisplayModule();
      const auto identity = m && m->module_() ? m->module_()->string_view() : "";
      constexpr std::string_view hash = "0123456789abcdfghijklmnpqrsvwxyz";
      constexpr std::string_view name = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-._?=";
      if (!identity.starts_with("/nix/store/") || identity.size() < 45 || identity[43] != '-' ||
          identity.substr(11, 32).find_first_not_of(hash) != std::string_view::npos ||
          identity.substr(44).find_first_not_of(name) != std::string_view::npos)
        return "a module must name a Nix store directory";
      if (m->blobs() && m->blobs()->size() > 64) return "a module exceeds 64 blobs";
      size_t bytes = 0;
      std::set<std::string_view> names;
      if (m->blobs()) for (const auto* blob : *m->blobs()) {
        const auto id = blob && blob->name() ? blob->name()->string_view() : "";
        if (id.empty() || id.size() > 128 || id == "." || id == ".." ||
            id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string_view::npos ||
            !names.insert(id).second) return "a module blob name must be a unique id";
        if (!blob->data() || blob->data()->size() > (8u << 20) - bytes) return "module blobs exceed 8 MiB";
        bytes += blob->data()->size();
      }
      if (!m->config_json() || m->config_json()->size() > kMaxBytes) return "module config exceeds 1 MiB";
      try { (void)schema::capture(m->config_json()->string_view()); }
      catch (const std::exception&) { return "module config is not valid JSON"; }
      break;
    }
    default: return "unknown display kind";
  }
  return std::nullopt;
}
}  // namespace tsugumi::workerd
