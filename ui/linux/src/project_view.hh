// A project: while it starts, what hostd is doing; once open, the read-only
// Canvas, the run's state and the selected Card's log.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>

#include "canvas_view.hh"
#include "log_view.hh"
#include "project_session.hh"

namespace tsugumi::linux_ui {

class ProjectView {
 public:
  ProjectView();
  ~ProjectView();
  ProjectView(const ProjectView&) = delete;
  auto operator=(const ProjectView&) -> ProjectView& = delete;

  [[nodiscard]] auto widget() const -> GtkWidget* { return root_; }
  auto Show(std::shared_ptr<ProjectSession> session) -> void;
  [[nodiscard]] auto session() const -> const std::shared_ptr<ProjectSession>& {
    return session_;
  }
  // Selects a Card (PageShow names one), once the project has it.
  auto Reveal(const std::string& card) -> void;
  [[nodiscard]] auto canvas() -> CanvasView& { return canvas_; }

  std::function<void()> on_home;
  std::function<void()> on_changed;

 private:
  auto Update() -> void;
  static auto HomeClicked(GtkButton* button, gpointer self) -> void;
  static auto RunClicked(GtkButton* button, gpointer self) -> void;
  static auto StopClicked(GtkButton* button, gpointer self) -> void;

  GtkWidget* root_;
  GtkWidget* home_;
  GtkWidget* title_;
  GtkWidget* status_;
  GtkWidget* run_;
  GtkWidget* stop_;
  GtkWidget* pages_;
  GtkWidget* spinner_;
  GtkWidget* message_;
  GtkWidget* detail_;
  CanvasView canvas_;
  LogView log_;
  std::shared_ptr<ProjectSession> session_;
  std::string reveal_;
  std::string shown_run_;
};

}  // namespace tsugumi::linux_ui
