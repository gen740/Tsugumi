// A Card's log in the current run: one stream at a time, read in windows
// (LogWindow) and followed while it grows.
#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string>

#include "log_window.hh"
#include "project_session.hh"

namespace tsugumi::linux_ui {

class LogView {
 public:
  LogView();
  ~LogView();
  LogView(const LogView&) = delete;
  auto operator=(const LogView&) -> LogView& = delete;

  [[nodiscard]] auto widget() const -> GtkWidget* { return root_; }
  // Shows card's log ("" shows none) of session's current run.
  auto Show(std::weak_ptr<ProjectSession> session, const std::string& card)
      -> void;
  // The run changed (another run, or it ended).
  auto RunChanged() -> void;
  auto Notice(const LogNotice& notice) -> void;
  [[nodiscard]] auto text() const -> const std::string& {
    return window_.text();
  }

 private:
  auto Reset() -> void;
  auto Read(const LogRequest& request) -> void;
  auto Render() -> void;
  [[nodiscard]] auto stream() const -> Tsugumi::Data::LogStream;
  static auto StreamChanged(GObject* dropdown, GParamSpec* spec, gpointer self)
      -> void;
  static auto EarlierClicked(GtkButton* button, gpointer self) -> void;

  GtkWidget* root_;
  GtkWidget* title_;
  GtkWidget* streams_;
  GtkWidget* earlier_;
  GtkWidget* follow_;
  GtkWidget* text_view_;
  GtkWidget* scroll_;
  std::weak_ptr<ProjectSession> session_;
  std::string card_;
  std::string run_;
  LogWindow window_;
  // Reads are one at a time; a read asked for meanwhile goes after it.
  bool reading_ = false;
  bool read_again_ = false;
  // Replies for a log no longer shown are dropped.
  uint64_t generation_ = 0;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace tsugumi::linux_ui
