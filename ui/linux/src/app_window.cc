#include "app_window.hh"

#include <climits>
#include <cstdlib>
#include <utility>
#include <vector>

#include "strings.hh"

namespace tsugumi::linux_ui {
namespace H = Tsugumi::Host;

namespace {

// The folder as hostd names a project (realpath), until hostd says.
auto HostPath(const std::string& path) -> std::string {
  char* real = ::realpath(path.c_str(), nullptr);
  if (!real) return path;
  std::string out = real;
  std::free(real);  // NOLINT(cppcoreguidelines-no-malloc): realpath's buffer
  return out;
}

auto NewPageName() -> std::string {
  gchar* id = g_uuid_string_random();
  std::string out = id;
  g_free(id);
  return out;
}

}  // namespace

auto AppWindow::Create(GtkApplication* app, std::shared_ptr<HostLink> host)
    -> AppWindow* {
  return new AppWindow(app, std::move(host));
}

AppWindow::AppWindow(GtkApplication* app, std::shared_ptr<HostLink> host)
    : window_(GTK_WINDOW(gtk_application_window_new(app))),
      host_(std::move(host)),
      page_(NewPageName()),
      stack_(gtk_stack_new()) {
  gtk_window_set_title(window_, "Tsugumi");
  gtk_window_set_default_size(window_, 1200, 800);
  gtk_stack_add_named(GTK_STACK(stack_), home_.widget(), "home");
  gtk_stack_add_named(GTK_STACK(stack_), project_.widget(), "project");
  gtk_window_set_child(window_, stack_);

  home_.on_open = [this](const std::string& path) { OpenProject(path); };
  project_.on_home = [this] { ShowHome(); };
  project_.on_changed = [this] {
    const auto& session = project_.session();
    const std::string title = session && !session->title().empty()
                                  ? session->title() + " — Tsugumi"
                                  : "Tsugumi";
    gtk_window_set_title(window_, title.c_str());
    Report();
  };
  listener_ =
      host_->Listen([this](const H::Event& event) { HostEvent(event); });
  g_signal_connect(window_, "destroy", G_CALLBACK(&AppWindow::Destroyed), this);
  ShowHome();
}

AppWindow::~AppWindow() { host_->Unlisten(listener_); }

auto AppWindow::Destroyed(GtkWidget* /*widget*/, gpointer self) -> void {
  auto* window = static_cast<AppWindow*>(self);
  window->host_->ClosePage(window->page_);
  delete window;
}

auto AppWindow::Report() -> void {
  const auto& session = project_.session();
  host_->ReportPage(page_, session ? session->path() : "");
}

auto AppWindow::OpenProject(const std::string& path) -> void {
  auto session = std::make_shared<ProjectSession>(host_, HostPath(path));
  project_.Show(session);
  gtk_stack_set_visible_child_name(GTK_STACK(stack_), "project");
  session->Open();
}

auto AppWindow::ShowHome() -> void {
  project_.Show(nullptr);
  gtk_window_set_title(window_, "Tsugumi");
  gtk_stack_set_visible_child_name(GTK_STACK(stack_), "home");
  Report();
  RefreshRecent();
}

auto AppWindow::RefreshRecent() -> void {
  host_->Call<H::RecentList>(
      H::HostRecentListRequestT{}, [this, alive = std::weak_ptr<int>(alive_)](
                                       const Reply<H::RecentList>& reply) {
        if (alive.expired()) return;
        if (!reply.ok()) {
          home_.ShowError(reply.error);
          return;
        }
        home_.ShowError("");
        std::vector<RecentEntry> recent;
        if (reply.value->projects())
          for (const auto* project : *reply.value->projects())
            recent.push_back({project->path()->str(), project->name()->str()});
        home_.ShowRecent(std::move(recent));
      });
}

// hostd chose this window to show a project (it was on Home), or tells the
// window that shows it to come forward, with a Card to reveal.
auto AppWindow::HostEvent(const H::Event& event) -> void {
  const auto* show = event.value_as_PageShow();
  if (!show || !show->page() || show->page()->str() != page_) return;
  const auto project = show->project()->str();
  const auto& session = project_.session();
  if (!session) {
    OpenProject(project);
  } else if (session->path() != project) {
    // This window took another project meanwhile: hostd learns it from the
    // report.
    return;
  }
  if (show->card() && show->card()->size() > 0)
    project_.Reveal(show->card()->str());
  gtk_window_present(window_);
}

}  // namespace tsugumi::linux_ui
