#include "host/layout_file.hh"

#include <cmath>

#include "host/layout_service.hh"
#include "tsugumi/document/document.hh"
#include "tsugumi/document/text.hh"
#include "tsugumi/protocol/layout_document_generated.h"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {
namespace {
using document_text::Schema;

template <class T>
void check_position(const T& place) {
  const auto x = place.x(), y = place.y();
  if (x.has_value() != y.has_value() ||
      (x && (!std::isfinite(*x) || !std::isfinite(*y))))
    throw LayoutFailure("layout: position needs two finite coordinates");
}

void check_coordinates(const Tsugumi::Layout::Layout& layout) {
  const auto boundary = [](const auto* bars) {
    if (!bars) return;
    if (const auto* input = bars->inputs()) check_position(*input);
    if (const auto* output = bars->outputs()) check_position(*output);
  };
  if (const auto* cards = layout.cards())
    for (const auto* card : *cards) check_position(*card);
  if (const auto* notes = layout.notes())
    for (const auto* note : *notes) {
      check_position(*note);
      for (const auto size : {note->width(), note->height(), note->fontSize()})
        if (size && !std::isfinite(*size))
          throw LayoutFailure("layout: note sizes must be finite");
    }
  if (const auto* boundaries = layout.boundaries())
    for (const auto* entry : *boundaries) boundary(entry);
  boundary(layout.boundary());
}

// An array of objects with a key field <-> an object keyed by it.
auto keyed(const std::string& array, const char* key) -> std::string {
  json::Members out;
  for (const auto& raw : json::Array(array)) {
    auto entry = json::Object(raw);
    const auto name = json::Text(entry, key);
    entry.erase(key);
    out[name] = json::Encode(entry);
  }
  return json::Encode(out);
}

auto listed(const std::string& object, const char* key,
            const std::string& where) -> std::string {
  std::vector<std::string> out;
  for (auto [name, raw] : json::Object(object, where)) {
    auto entry = json::Object(raw, where + "." + name);
    entry[key] = json::Quote(name);
    out.push_back(json::Encode(entry));
  }
  return json::Encode(out);
}
}  // namespace

auto LayoutView(const std::string& layout,
                const std::map<std::string, std::string>& notes)
    -> std::string {
  if (layout.empty() && notes.empty()) return "{}";
  json::Members view;
  if (!layout.empty()) {
    if (!document_text::verify(Schema::Layout, layout))
      throw LayoutFailure("invalid layout FlatBuffer");
    view = json::Object(document_text::print(Schema::Layout, layout));
    if (view.contains("cards")) view["cards"] = keyed(view.at("cards"), "key");
    if (view.contains("boundaries"))
      view["boundaries"] = keyed(view.at("boundaries"), "scope");
  }
  if (view.contains("notes")) {
    std::vector<std::string> list;
    for (const auto& raw : json::Array(view.at("notes"))) {
      auto note = json::Object(raw);
      if (const auto text = notes.find(json::Text(note, "id"));
          text != notes.end())
        note["text"] = json::Quote(text->second);
      list.push_back(json::Encode(note));
    }
    view["notes"] = json::Encode(list);
  }
  return json::Encode(view);
}

auto SplitLayoutView(const std::string& view) -> LayoutFiles {
  LayoutFiles out;
  try {
    auto layout = json::Object(view, "layout");
    if (layout.empty()) return out;
    if (layout.contains("cards"))
      layout["cards"] = listed(layout.at("cards"), "key", "layout.cards");
    if (layout.contains("boundaries"))
      layout["boundaries"] =
          listed(layout.at("boundaries"), "scope", "layout.boundaries");
    if (layout.contains("notes")) {
      std::vector<std::string> list;
      for (const auto& raw : json::Array(layout.at("notes"), "layout.notes")) {
        auto note = json::Object(raw, "layout.notes");
        const auto id = json::Text(note, "id");
        if (!valid_id(id))
          throw LayoutFailure("layout: a note id must match [A-Za-z0-9_.-]");
        if (note.contains("text")) {
          out.notes[id] = json::Text(note, "text");
          note.erase("text");
        }
        list.push_back(json::Encode(note));
      }
      layout["notes"] = json::Encode(list);
    }
    out.layout = document_text::parse(Schema::Layout, "layout.bin",
                                      json::Encode(layout));
    check_coordinates(
        document_text::root<Tsugumi::Layout::Layout>(out.layout));
  } catch (const LayoutFailure&) {
    throw;
  } catch (const std::exception& error) {
    throw LayoutFailure(error.what());
  }
  return out;
}

auto NotePath(const std::string& prefix, const std::string& id)
    -> std::string {
  return prefix + "notes/" + id + ".md";
}

auto NoteId(const std::string& prefix, const std::string& path)
    -> std::optional<std::string> {
  if (!path.starts_with(prefix + "notes/") || !path.ends_with(".md"))
    return std::nullopt;
  auto id = path.substr(prefix.size() + 6, path.size() - prefix.size() - 9);
  if (!valid_id(id)) return std::nullopt;
  return id;
}

}  // namespace tsugumi::host
