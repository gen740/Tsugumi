// Presentation validation belongs to the common Card ABI, inside the worker.
#pragma once

#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "tsugumi/protocol/data_generated.h"
#include "tsugumi/util/json.hh"
#include "tsugumi/util/text.hh"

namespace tsugumi::workerd {
// The native ABI has checked the pointers, sizes and shape before packing.
inline auto validate_display(const Tsugumi::Data::Display& display)
    -> std::optional<std::string> {
  namespace D = Tsugumi::Data;
  const auto utf8 = [](const flatbuffers::String* s) {
    return s && text::valid_utf8(s->string_view());
  };
  switch (display.value_type()) {
    case D::DisplayValue::DisplayText: {
      const auto* t = display.value_as_DisplayText();
      if (!utf8(t->text())) return "display text is not UTF-8";
      break;
    }
    case D::DisplayValue::DisplayScalar: {
      const auto* s = display.value_as_DisplayScalar();
      if (!s || !s->value() || !std::isfinite(*s->value()))
        return "display scalar must be finite";
      if (s->label() && !utf8(s->label())) return "display label is not UTF-8";
      break;
    }
    case D::DisplayValue::DisplayTable: {
      const auto* t = display.value_as_DisplayTable();
      for (const auto* c : *t->columns())
        if (!utf8(c)) return "table column names must be UTF-8";
      for (const auto* r : *t->rows()) {
        for (const auto* c : *r->cells())
          if (!utf8(c)) return "table cells must be UTF-8";
      }
      break;
    }
    case D::DisplayValue::DisplayImage: {
      const auto* im = display.value_as_DisplayImage();
      const std::string_view bytes(
          reinterpret_cast<const char*>(im->data()->data()),
          im->data()->size());
      const auto media =
          im->media_type() ? im->media_type()->string_view() : "";
      const bool png =
          bytes.size() > 8 && bytes.starts_with("\x89PNG\r\n\x1a\n");
      const bool jpeg = bytes.size() > 3 && bytes.starts_with("\xff\xd8");
      if (media == "image/png" ? !png : media == "image/jpeg" ? !jpeg : true)
        return "images must be PNG or JPEG data matching media_type";
      if (im->alt() && !utf8(im->alt())) return "image alt text is not UTF-8";
      break;
    }
    case D::DisplayValue::DisplayPointCloud: {
      const auto* p = display.value_as_DisplayPointCloud();
      for (size_t i = 0; i < p->x()->size(); ++i)
        if (!std::isfinite(p->x()->Get(i)) || !std::isfinite(p->y()->Get(i)) ||
            !std::isfinite(p->z()->Get(i)))
          return "point coordinates must be finite";
      break;
    }
    case D::DisplayValue::DisplayModule: {
      const auto* m = display.value_as_DisplayModule();
      const auto identity =
          m && m->module_() ? m->module_()->string_view() : "";
      constexpr std::string_view hash = "0123456789abcdfghijklmnpqrsvwxyz";
      constexpr std::string_view name =
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-._?"
          "=";
      if (!identity.starts_with("/nix/store/") || identity.size() < 45 ||
          identity[43] != '-' ||
          identity.substr(11, 32).find_first_not_of(hash) !=
              std::string_view::npos ||
          identity.substr(44).find_first_not_of(name) != std::string_view::npos)
        return "a module must name a Nix store directory";
      std::set<std::string_view> names;
      if (m->blobs())
        for (const auto* blob : *m->blobs()) {
          const auto id =
              blob && blob->name() ? blob->name()->string_view() : "";
          if (id.empty() || id == "." || id == ".." ||
              id.find_first_not_of(
                  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRS"
                  "TUVWXYZ0123456789_.-") != std::string_view::npos ||
              !names.insert(id).second)
            return "a module blob name must be a unique id";
        }
      try {
        (void)schema::capture(m->config_json()->string_view());
      } catch (const std::exception&) {
        return "module config is not valid JSON";
      }
      break;
    }
    default:
      return "unknown display kind";
  }
  return std::nullopt;
}
}  // namespace tsugumi::workerd
