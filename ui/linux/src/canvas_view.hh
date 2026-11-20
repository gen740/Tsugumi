// The read-only Canvas: the scene fitted into the view, each Card with its
// state in the current run. Clicking a Card selects it.
#pragma once

#include <cairo.h>
#include <gtk/gtk.h>

#include <functional>
#include <string>

#include "canvas_scene.hh"
#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi::linux_ui {

using CardStateOf = std::function<Tsugumi::Data::CardState(const std::string&)>;

// The Japanese source text of a Card's state in a run ("" when it has none).
auto CardStateText(Tsugumi::Data::CardState state) -> const char*;
auto RunStateText(Tsugumi::Data::RunState state) -> const char*;

struct CanvasColors {
  GdkRGBA text{0, 0, 0, 1};
  GdkRGBA accent{0.21, 0.52, 0.89, 1};
};

// Draws scene into a width x height area: used by the view and its test.
auto DrawScene(cairo_t* cr, int width, int height, const Scene& scene,
               const CardStateOf& state_of, const std::string& selected,
               const CanvasColors& colors) -> void;
// The Card at a point of the area, as DrawScene placed it.
auto CardAtPoint(int width, int height, const Scene& scene, double x, double y)
    -> const SceneCard*;

class CanvasView {
 public:
  CanvasView();
  ~CanvasView();
  CanvasView(const CanvasView&) = delete;
  auto operator=(const CanvasView&) -> CanvasView& = delete;

  [[nodiscard]] auto widget() const -> GtkWidget* { return area_; }
  // scene must stay valid until the next call.
  auto Show(const Scene* scene, CardStateOf state_of) -> void;
  auto Select(const std::string& card) -> void;
  [[nodiscard]] auto selected() const -> const std::string& {
    return selected_;
  }
  [[nodiscard]] auto draws() const -> int { return draws_; }

  std::function<void(const std::string&)> on_select;

 private:
  static auto Draw(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                   gpointer self) -> void;
  static auto Pressed(GtkGestureClick* gesture, int presses, double x, double y,
                      gpointer self) -> void;

  GtkWidget* area_;
  GtkGesture* click_;  // owned by area_
  const Scene* scene_ = nullptr;
  CardStateOf state_of_;
  std::string selected_;
  int draws_ = 0;
};

}  // namespace tsugumi::linux_ui
