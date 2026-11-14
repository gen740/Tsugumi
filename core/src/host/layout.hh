// Arrange and Untangle Wires: the two operations that rewrite where a
// project's Cards sit (layout.json) are worked out here, once, for every
// client (LayoutArrange / LayoutUntangle of protocol/host.fbs).
//
// Both are functions of a Scene, what a canvas draws in one scope: a Card's
// size and the places of its ports depend on how the window draws them and on
// its grid, so the client measures and the core decides. Nothing here reads or
// writes a document: the answer is a proposal the client saves as one edit.
//
// Everything is plain arithmetic on doubles in a fixed order (no map or set is
// iterated, sorts are total orders, and the files are built without floating
// point contraction), so the same scene gives the same answer on every machine:
// tests/fixtures/layout-golden.json holds it to the last digit.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace tsugumi::layout {

struct Port {
  std::string name;
  // Where the port is on its Card, from the Card's top-left corner.
  double x = 0;
  double y = 0;
};

struct Card {
  std::string id;
  // Where the Card is drawn now.
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
  // In the order drawn.
  std::vector<Port> inputs;
  std::vector<Port> outputs;
  // Its ports may be drawn in another order.
  bool reorderable = false;
};

struct Wire {
  std::string from;
  std::string from_port;
  std::string to;
  std::string to_port;
};

// An output frame (`id` is its Card's) or a note.
struct Box {
  std::string id;
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

struct Point {
  double x = 0;
  double y = 0;
};

struct Rect {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

// One port of a Card in Card and the inner Card it goes to or comes from.
struct BarEnd {
  std::string name;
  std::string card;
  std::string port;
};

// A bar of a Card in Card's inner canvas; `at` when it was put there by hand.
struct Bar {
  std::vector<BarEnd> ends;
  std::optional<Point> at;
};

struct Scene {
  bool top_to_bottom = true;
  double grid = 20;
  // The grid is on: Cards are placed on it.
  bool snap = false;
  std::vector<Card> cards;
  std::vector<Wire> wires;
  std::vector<Box> frames;
  std::vector<Box> notes;
  Bar inputs;
  Bar outputs;
};

struct Place {
  std::string id;
  double x = 0;
  double y = 0;
};

struct PortOrder {
  std::string id;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
};

struct Proposal {
  std::vector<Place> cards;
  std::vector<Place> frames;
  std::vector<Place> notes;
  // Only the Cards whose order changes.
  std::vector<PortOrder> port_orders;
};

// Places the Cards, output frames and notes, gives ports the order that
// crosses fewer wires, and places the Cards once more for that order. The bars
// are taken to follow the Cards.
auto arrange(const Scene& scene) -> Proposal;

// Reorders ports where the wires then cross less; no Card moves. With `around`
// (the Cards a drag just moved) only they and the Cards wired to them may
// change.
auto untangle(const Scene& scene, const std::vector<std::string>& around)
    -> Proposal;

// ---- The steps (layout_place.cc, layout_wires.cc), also used by the tests
// ----

// A layered drawing of the graph, the way a person lays one out by hand: wires
// that can be straight are, what comes after a fork stays in its own column,
// gaps follow the real size of each Card, and output frames and notes go in
// free room near what they belong to. Positions of the scene are not read.
auto place(const Scene& scene) -> Proposal;

// Where the canvas draws each wire (its points), given the Cards and frames in
// its way: a wire going along the flow whose curve touches no Card stays that
// curve (sampled), any other takes the cheapest path on a grid of lines beside
// the Cards. The same search as the clients' WireRouter, whose routes it must
// give (tests/fixtures/wire-routes.json).
struct WireEnds {
  Point a;
  Point b;
};
auto route_wires(bool top_to_bottom, const std::vector<Rect>& obstacles,
                 const std::vector<WireEnds>& wires)
    -> std::vector<std::vector<Point>>;

// Pairs of wires that cross, each counted once.
auto crossing_pairs(const std::vector<std::vector<Point>>& paths,
                    const std::vector<WireEnds>& ends) -> int;

}  // namespace tsugumi::layout
