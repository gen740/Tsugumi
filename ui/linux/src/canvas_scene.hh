// What the read-only Canvas draws, taken from a project snapshot: the Cards
// of the top-level Workflow at the places hostd completed (Host
// LayoutCompleteRequest), and the wires between them.
//
// A Card's size, shape and port positions are not here: the Web UI and the
// Mac app work them out themselves (CardGeometry), and this UI waits for
// hostd to give them. Until then every Card is drawn as a box of one size.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tsugumi::linux_ui {

struct Definition {
  std::string id;
  std::string card_json;  // definitions/<id>/_card.json
  std::string meta_json;  // its metadata (name, ...), may be empty
  // The core's first error about this Card (its ResolvedCard), may be empty.
  std::string diagnostic;
};

struct SceneCard {
  std::string id;
  std::string name;
  double x = 0;
  double y = 0;
  // Its _card.json has no type, one this build does not know or one that
  // does not match "external" (or there is no definition): drawn as a
  // definition error with the core's diagnostic, and never run.
  bool definition_error = false;
  std::string diagnostic;
};

struct SceneWire {
  std::string from;
  std::string from_port;
  std::string to;
  std::string to_port;
};

struct Box {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
  [[nodiscard]] auto Contains(double px, double py) const -> bool {
    return px >= x && px <= x + width && py >= y && py <= y + height;
  }
};

struct Point {
  double x = 0;
  double y = 0;
};

struct Scene {
  bool top_to_bottom = true;
  std::vector<SceneCard> cards;
  std::vector<SceneWire> wires;

  [[nodiscard]] auto Find(std::string_view id) const -> const SceneCard*;
  // The box a Card is drawn in (one size for every Card, see above).
  [[nodiscard]] static auto CardBox(const SceneCard& card) -> Box;
  // Where a wire leaves its Card and reaches the next one: the middle of the
  // downstream and upstream edges, in the direction the Cards run.
  [[nodiscard]] auto WireEnds(const SceneWire& wire) const
      -> std::pair<Point, Point>;
  // Everything drawn; an empty box for an empty Workflow.
  [[nodiscard]] auto Bounds() const -> Box;
  // The Card drawn at a point, or nullptr.
  [[nodiscard]] auto CardAt(double x, double y) const -> const SceneCard*;
};

// Whether a _card.json states one of the Card types (a subgraph exactly when
// it names an "external"). Only recognises the value; the core validates.
auto StatesKnownType(std::string_view card_json) -> bool;

// LayoutCompleteRequest's definitions_json: each Card id with the text of its
// _card.json.
auto CompletionDefinitions(const std::vector<Definition>& definitions)
    -> std::string;

// project_json: tsugumi.json; layout_json: the layout hostd completed.
auto BuildScene(std::string_view project_json, std::string_view layout_json,
                const std::vector<Definition>& definitions) -> Scene;

// tsugumi.json's name, or fallback.
auto ProjectTitle(std::string_view project_json, std::string fallback)
    -> std::string;

}  // namespace tsugumi::linux_ui
