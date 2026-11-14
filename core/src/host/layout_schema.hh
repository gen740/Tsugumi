#pragma once

#include "schema.hh"

namespace tsugumi::host::layout_schema {
using namespace tsugumi::schema;

// ---- layout.json -------------------------------------------------------------

struct Position {
  Opt<"x", Number> x;
  Opt<"y", Number> y;
  Opt<"collapsed", v::Bool> collapsed;  // display only; the Card still runs
  Opt<"inputOrder", v::Array<v::String>> input_order;
  Opt<"outputOrder", v::Array<v::String>> output_order;  // visual order by name; connections stay unchanged
};

struct StickyNote {
  Opt<"id", v::String> id;
  Opt<"scope", v::String> scope;  // graph scope path key ("" = root)
  Opt<"text", v::String> text;    // Markdown
  Opt<"color", v::String> color;
  Opt<"x", Number> x;
  Opt<"y", Number> y;
  Opt<"width", Number> width;
  Opt<"height", Number> height;
  Opt<"fontSize", Number> font_size;  // text size of the note (default 13)
};

struct DisplayPanel {
  Opt<"x", Number> x;
  Opt<"y", Number> y;
  Opt<"width", Number> width;
  Opt<"height", Number> height;
  Opt<"hidden", v::Bool> hidden;  // frame removed from the canvas; the Card is unchanged
};

struct GroupPort {
  Opt<"card", v::String> card;
  Opt<"port", v::String> port;
};

// Visual grouping only: never changes execution order or the graph.
struct CardGroup {
  Opt<"id", v::String> id;
  Opt<"scope", v::String> scope;
  Opt<"name", v::String> name;
  Opt<"color", v::String> color;
  Opt<"cards", v::Array<v::String>> cards;              // instance ids within scope
  Opt<"outputs", v::Array<Obj<GroupPort>>> outputs;     // outputs shown on the group frame
};

// Where the inner canvas of a Card in Card shows its boundary bars ("This Card's
// input" / "output"); placed automatically when absent.
struct BoundaryLayout {
  Opt<"inputs", Obj<Position>> inputs;
  Opt<"outputs", Obj<Position>> outputs;
};

struct Layout {
  // The direction the Cards were laid out in: "topToBottom" or "leftToRight".
  Opt<"flow", v::String> flow;
  Opt<"cards", v::Mapping<v::String, Obj<Position>>> cards;  // card path key -> position
  Opt<"boundaries", v::Mapping<v::String, Obj<BoundaryLayout>>> boundaries;  // scope path key -> bars
  Opt<"boundary", Obj<BoundaryLayout>> boundary;  // a Card in Card's own layout: its boundary bars
  Opt<"notes", v::Array<Obj<StickyNote>>> notes;
  Opt<"displays", v::Mapping<v::String, Obj<DisplayPanel>>> displays;  // card path key -> panel
  Opt<"groups", v::Array<Obj<CardGroup>>> groups;
};

}  // namespace tsugumi::host::layout_schema
