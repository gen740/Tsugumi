// tsugumi-gtk [FOLDER...]: the Linux desktop UI. It starts or finds the
// user's hostd as the tsugumi command does; each window is Home or a project.
#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

#include "app_window.hh"
#include "host_link.hh"
#include "strings.hh"

namespace {
namespace ui = tsugumi::linux_ui;

struct App {
  GtkApplication* application = nullptr;
  std::shared_ptr<ui::HostLink> host;
  bool ready = false;
  std::string error;
  // Windows asked for before the host answered ("" for Home).
  std::vector<std::string> pending;
};

auto ShowFailure(App& app) -> void {
  GtkWidget* window = gtk_application_window_new(app.application);
  gtk_window_set_title(GTK_WINDOW(window), "Tsugumi");
  gtk_window_set_default_size(GTK_WINDOW(window), 640, 240);
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  for (auto* side : {&gtk_widget_set_margin_top, &gtk_widget_set_margin_bottom,
                     &gtk_widget_set_margin_start, &gtk_widget_set_margin_end})
    side(box, 24);
  GtkWidget* title =
      gtk_label_new(ui::L("Tsugumi のホストを起動できませんでした").c_str());
  gtk_widget_add_css_class(title, "title-3");
  GtkWidget* detail = gtk_label_new(app.error.c_str());
  gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
  gtk_label_set_selectable(GTK_LABEL(detail), TRUE);
  gtk_box_append(GTK_BOX(box), title);
  gtk_box_append(GTK_BOX(box), detail);
  gtk_window_set_child(GTK_WINDOW(window), box);
  gtk_window_present(GTK_WINDOW(window));
}

auto Open(App& app, const std::string& folder) -> void {
  if (!app.ready) {
    app.pending.push_back(folder);
    return;
  }
  if (!app.error.empty()) return ShowFailure(app);
  auto* window = ui::AppWindow::Create(app.application, app.host);
  if (!folder.empty()) window->OpenProject(folder);
  gtk_window_present(window->window());
}

auto Startup(GApplication* application, gpointer data) -> void {
  auto& app = *static_cast<App*>(data);
  // Until the host answers, the application stays even without a window.
  g_application_hold(application);
  char* executable = g_file_read_link("/proc/self/exe", nullptr);
  app.host =
      std::make_shared<ui::HostLink>(executable ? executable : "tsugumi-gtk");
  g_free(executable);
  app.host->Start([&app, application](const std::string& error) {
    app.ready = true;
    app.error = error;
    ui::SetStrings(ui::Strings(app.host->language(), ui::EnglishDictionary()));
    const auto pending = std::move(app.pending);
    app.pending.clear();
    for (const auto& folder : pending) Open(app, folder);
    g_application_release(application);
  });

  GSimpleAction* new_window = g_simple_action_new("new-window", nullptr);
  g_signal_connect(new_window, "activate",
                   G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
                     Open(*static_cast<App*>(data), "");
                   }),
                   &app);
  g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(new_window));
  g_object_unref(new_window);
  const char* accels[] = {"<Control>n", nullptr};
  gtk_application_set_accels_for_action(GTK_APPLICATION(application),
                                        "app.new-window", accels);
}

auto Activate(GApplication* application, gpointer data) -> void {
  auto& app = *static_cast<App*>(data);
  if (GtkWindow* window =
          gtk_application_get_active_window(GTK_APPLICATION(application))) {
    gtk_window_present(window);
    return;
  }
  Open(app, "");
}

auto OpenFiles(GApplication* /*application*/, GFile** files, int count,
               const char* /*hint*/, gpointer data) -> void {
  auto& app = *static_cast<App*>(data);
  for (int i = 0; i < count; ++i) {
    char* path = g_file_get_path(files[i]);  // NOLINT: GLib's array
    if (path) Open(app, path);
    g_free(path);
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  App app;
  app.application =
      gtk_application_new("dev.tsugumi.Tsugumi", G_APPLICATION_HANDLES_OPEN);
  g_signal_connect(app.application, "startup", G_CALLBACK(&Startup), &app);
  g_signal_connect(app.application, "activate", G_CALLBACK(&Activate), &app);
  g_signal_connect(app.application, "open", G_CALLBACK(&OpenFiles), &app);
  const int status =
      g_application_run(G_APPLICATION(app.application), argc, argv);
  g_object_unref(app.application);
  return status;
}
