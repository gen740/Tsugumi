#include "canvas_view.hh"

#include <pango/pangocairo.h>

#include <algorithm>
#include <numbers>
#include <utility>

#include "strings.hh"

namespace tsugumi::linux_ui {
namespace D = Tsugumi::Data;

namespace {

constexpr double kMargin = 32;
constexpr double kRadius = 10;

struct Fit {
  double scale = 1;
  double dx = 0;
  double dy = 0;
};

// The whole Workflow in view, never larger than its own size.
auto FitScene(int width, int height, const Scene& scene) -> Fit {
  const auto bounds = scene.Bounds();
  if (bounds.width <= 0 || bounds.height <= 0) return {};
  const double scale =
      std::min({(width - (2 * kMargin)) / bounds.width,
                (height - (2 * kMargin)) / bounds.height, 1.0});
  const double fitted = std::max(scale, 0.05);
  return {fitted, ((width - (bounds.width * fitted)) / 2) - (bounds.x * fitted),
          ((height - (bounds.height * fitted)) / 2) - (bounds.y * fitted)};
}

auto StateColor(D::CardState state, const CanvasColors& colors) -> GdkRGBA {
  switch (state) {
    case D::CardState::succeeded:
      return {0.20, 0.66, 0.33, 1};
    case D::CardState::failed:
      return {0.86, 0.24, 0.22, 1};
    case D::CardState::running:
    case D::CardState::preparing:
      return colors.accent;
    case D::CardState::waiting:
    case D::CardState::ready:
      return {0.90, 0.62, 0.10, 1};
    default:
      return {colors.text.red, colors.text.green, colors.text.blue, 0.45};
  }
}

auto RoundedBox(cairo_t* cr, const Box& box) -> void {
  const double r = kRadius;
  cairo_new_sub_path(cr);
  cairo_arc(cr, box.x + box.width - r, box.y + r, r, -std::numbers::pi / 2, 0);
  cairo_arc(cr, box.x + box.width - r, box.y + box.height - r, r, 0,
            std::numbers::pi / 2);
  cairo_arc(cr, box.x + r, box.y + box.height - r, r, std::numbers::pi / 2,
            std::numbers::pi);
  cairo_arc(cr, box.x + r, box.y + r, r, std::numbers::pi,
            3 * std::numbers::pi / 2);
  cairo_close_path(cr);
}

auto SetColor(cairo_t* cr, const GdkRGBA& color, double alpha = 1) -> void {
  cairo_set_source_rgba(cr, color.red, color.green, color.blue,
                        color.alpha * alpha);
}

auto Label(cairo_t* cr, const std::string& text, const Box& box, double y,
           int size, bool bold) -> void {
  PangoLayout* layout = pango_cairo_create_layout(cr);
  PangoFontDescription* font = pango_font_description_new();
  pango_font_description_set_family(font, "Sans");
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  if (bold) pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_width(layout,
                         static_cast<int>((box.width - 20) * PANGO_SCALE));
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
  pango_layout_set_text(layout, text.c_str(), -1);
  cairo_move_to(cr, box.x + 10, y);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(font);
  g_object_unref(layout);
}

}  // namespace

auto CardStateText(D::CardState state) -> const char* {
  switch (state) {
    case D::CardState::not_started:
      return "未実行";
    case D::CardState::waiting:
      return "入力待ち";
    case D::CardState::ready:
      return "実行待ち";
    case D::CardState::preparing:
      return "準備中";
    case D::CardState::running:
      return "実行中";
    case D::CardState::succeeded:
      return "成功";
    case D::CardState::failed:
      return "失敗";
    case D::CardState::blocked:
      return "ブロック";
    case D::CardState::cancelled:
      return "止めました";
    case D::CardState::skipped:
      return "スキップ";
    default:
      return "";
  }
}

auto RunStateText(D::RunState state) -> const char* {
  switch (state) {
    case D::RunState::queued:
      return "実行待ち";
    case D::RunState::preparing:
      return "準備中";
    case D::RunState::running:
      return "実行中";
    case D::RunState::cancelling:
      return "停止中";
    case D::RunState::succeeded:
      return "成功";
    case D::RunState::failed:
      return "失敗";
    case D::RunState::cancelled:
      return "止めました";
    default:
      return "";
  }
}

auto DrawScene(cairo_t* cr, int width, int height, const Scene& scene,
               const CardStateOf& state_of, const std::string& selected,
               const CanvasColors& colors) -> void {
  const auto fit = FitScene(width, height, scene);
  cairo_save(cr);
  cairo_translate(cr, fit.dx, fit.dy);
  cairo_scale(cr, fit.scale, fit.scale);

  // A plain curve for now: drawn routes are each UI's own work (hostd's
  // route_wires is for Untangle, which edits layout.json).
  // Wires under the Cards: from the downstream edge to the upstream one.
  cairo_set_line_width(cr, 2);
  SetColor(cr, colors.text, 0.45);
  for (const auto& wire : scene.wires) {
    const auto [from, to] = scene.WireEnds(wire);
    cairo_move_to(cr, from.x, from.y);
    if (scene.top_to_bottom) {
      const double mid = (from.y + to.y) / 2;
      cairo_curve_to(cr, from.x, mid, to.x, mid, to.x, to.y);
    } else {
      const double mid = (from.x + to.x) / 2;
      cairo_curve_to(cr, mid, from.y, mid, to.y, to.x, to.y);
    }
    cairo_stroke(cr);
  }

  for (const auto& card : scene.cards) {
    const auto box = Scene::CardBox(card);
    const auto state = state_of ? state_of(card.id) : D::CardState::unspecified;
    const auto color = StateColor(state, colors);
    RoundedBox(cr, box);
    SetColor(cr, color, 0.12);
    cairo_fill_preserve(cr);
    SetColor(cr, card.id == selected ? colors.accent : color);
    cairo_set_line_width(cr, card.id == selected ? 4 : 2);
    cairo_stroke(cr);
    SetColor(cr, colors.text);
    Label(cr, card.name, box, box.y + 18, 15, true);
    if (const auto* text = CardStateText(state); *text != '\0') {
      SetColor(cr, color);
      Label(cr, L(text), box, box.y + 46, 13, false);
    }
  }
  cairo_restore(cr);
}

auto CardAtPoint(int width, int height, const Scene& scene, double x, double y)
    -> const SceneCard* {
  const auto fit = FitScene(width, height, scene);
  return scene.CardAt((x - fit.dx) / fit.scale, (y - fit.dy) / fit.scale);
}

CanvasView::CanvasView() : area_(gtk_drawing_area_new()) {
  g_object_ref_sink(area_);
  gtk_widget_set_hexpand(area_, TRUE);
  gtk_widget_set_vexpand(area_, TRUE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), &CanvasView::Draw,
                                 this, nullptr);
  click_ = gtk_gesture_click_new();
  g_signal_connect(click_, "pressed", G_CALLBACK(&CanvasView::Pressed), this);
  gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(click_));
}

CanvasView::~CanvasView() {
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), nullptr, nullptr,
                                 nullptr);
  g_signal_handlers_disconnect_by_data(click_, this);
  g_object_unref(area_);
}

auto CanvasView::Show(const Scene* scene, CardStateOf state_of) -> void {
  scene_ = scene;
  state_of_ = std::move(state_of);
  gtk_widget_queue_draw(area_);
}

auto CanvasView::Select(const std::string& card) -> void {
  selected_ = card;
  gtk_widget_queue_draw(area_);
}

auto CanvasView::Draw(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                      gpointer self) -> void {
  auto* view = static_cast<CanvasView*>(self);
  ++view->draws_;
  if (!view->scene_) return;
  CanvasColors colors;
  gtk_widget_get_color(GTK_WIDGET(area), &colors.text);
  DrawScene(cr, width, height, *view->scene_, view->state_of_, view->selected_,
            colors);
}

auto CanvasView::Pressed(GtkGestureClick* gesture, int /*presses*/, double x,
                         double y, gpointer self) -> void {
  auto* view = static_cast<CanvasView*>(self);
  if (!view->scene_) return;
  GtkWidget* area =
      gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  const auto* card =
      CardAtPoint(gtk_widget_get_width(area), gtk_widget_get_height(area),
                  *view->scene_, x, y);
  view->Select(card ? card->id : "");
  if (view->on_select) view->on_select(view->selected_);
}

}  // namespace tsugumi::linux_ui
