// layout.bin and the canvas notes, as the JSON view hostd and the UIs edit.
//
// A project stores where its Cards, notes and groups sit in layout.bin (a
// FlatBuffer of protocol/layout_document.fbs) and each note's Markdown in
// notes/<id>.md. hostd works on one JSON object for both, keyed like the
// rest of its document files under "layout.bin":
//   {"flow", "cards": {key: place}, "boundaries": {scope: bars},
//    "boundary": bars, "notes": [{id, text, ...}], "groups": [...]}
#pragma once

#include <map>
#include <optional>
#include <string>

namespace tsugumi::host {

struct LayoutFiles {
  std::string layout;                        // layout.bin; empty: none
  std::map<std::string, std::string> notes;  // note id -> Markdown
};

// The JSON view of a layout and its notes' text ("{}" for no layout).
auto LayoutView(const std::string& layout,
                const std::map<std::string, std::string>& notes = {})
    -> std::string;
// Splits a JSON view into layout.bin and the notes' text. Throws
// LayoutFailure when it does not fit the layout schema.
auto SplitLayoutView(const std::string& view) -> LayoutFiles;

// "notes/<id>.md" of a project prefix ("" or "externals/<p>/...").
auto NotePath(const std::string& prefix, const std::string& id)
    -> std::string;
// The note id of a notes/<id>.md under the prefix.
auto NoteId(const std::string& prefix, const std::string& path)
    -> std::optional<std::string>;

}  // namespace tsugumi::host
