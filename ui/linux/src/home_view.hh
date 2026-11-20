// Home: the recent projects hostd keeps (HostRecentListRequest) and a folder
// chooser to open another.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace tsugumi::linux_ui {

struct RecentEntry {
  std::string path;
  std::string name;
};

class HomeView {
 public:
  HomeView();
  ~HomeView();
  HomeView(const HomeView&) = delete;
  auto operator=(const HomeView&) -> HomeView& = delete;

  [[nodiscard]] auto widget() const -> GtkWidget* { return root_; }
  auto ShowRecent(std::vector<RecentEntry> recent) -> void;
  auto ShowError(const std::string& message) -> void;
  [[nodiscard]] auto recent() const -> const std::vector<RecentEntry>& {
    return recent_;
  }

  // A project folder was chosen.
  std::function<void(const std::string& path)> on_open;

 private:
  static auto Activated(GtkListBox* list, GtkListBoxRow* row, gpointer self)
      -> void;
  static auto Choose(GtkButton* button, gpointer self) -> void;
  static auto Chosen(GObject* dialog, GAsyncResult* result, gpointer self)
      -> void;

  GtkWidget* root_;
  GtkWidget* list_;
  GtkWidget* error_;
  GtkWidget* button_;
  std::vector<RecentEntry> recent_;
  // Cancels a folder chooser still open when Home goes away.
  GCancellable* choosing_;
};

}  // namespace tsugumi::linux_ui
