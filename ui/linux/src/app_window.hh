// A window: Home or one project, as a page of the host. hostd may show a
// project in a window on Home (PageShow, from an Agent's ShowRequest) and
// tells the window already showing it to come forward.
#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string>

#include "home_view.hh"
#include "host_link.hh"
#include "project_view.hh"

namespace tsugumi::linux_ui {

class AppWindow {
 public:
  // The window deletes itself when it is closed.
  static auto Create(GtkApplication* app, std::shared_ptr<HostLink> host)
      -> AppWindow*;

  [[nodiscard]] auto window() const -> GtkWindow* { return window_; }
  [[nodiscard]] auto page() const -> const std::string& { return page_; }
  [[nodiscard]] auto home() -> HomeView& { return home_; }
  [[nodiscard]] auto project() -> ProjectView& { return project_; }

  auto OpenProject(const std::string& path) -> void;
  auto ShowHome() -> void;
  auto RefreshRecent() -> void;

 private:
  AppWindow(GtkApplication* app, std::shared_ptr<HostLink> host);
  ~AppWindow();
  auto Report() -> void;
  auto HostEvent(const Tsugumi::Host::Event& event) -> void;
  static auto Destroyed(GtkWidget* widget, gpointer self) -> void;

  GtkWindow* window_;
  std::shared_ptr<HostLink> host_;
  std::string page_;
  GtkWidget* stack_;
  HomeView home_;
  ProjectView project_;
  int listener_ = 0;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace tsugumi::linux_ui
