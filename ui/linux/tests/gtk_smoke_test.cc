// The Linux UI on a real display (the Nix check uses GTK's Broadway backend).
//
// Without TSUGUMI_HOSTD: Home and the Canvas are built, shown and drawn.
// With it, also against a hostd this test starts: a remembered folder is
// listed on Home, an Agent's ShowRequest opens it in the window on Home
// (HostPageRequest, PageShow), the project opens with its Canvas and a Card's
// run is followed. TSUGUMI_GTK_TEST_PROJECT names that project's folder.
#include <gtk/gtk.h>
#include <signal.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "app_window.hh"
#include "canvas_view.hh"
#include "home_view.hh"
#include "host_link.hh"
#include "strings.hh"
#include "tsugumi/cli/launch.hh"
#include "tsugumi/cli/rpc.hh"

namespace {
namespace ui = tsugumi::linux_ui;
namespace D = Tsugumi::Data;
namespace H = Tsugumi::Host;

int failures = 0;

auto Check(bool ok, const std::string& what) -> void {
  std::fprintf(stderr, "%s: %s\n", ok ? "ok" : "FAILED", what.c_str());
  if (!ok) ++failures;
}

auto Env(const char* name) -> std::string {
  const char* value = std::getenv(name);
  return value ? value : "";
}

// Runs the main loop until ready() or the time is up.
auto WaitFor(const std::function<bool()>& ready, std::chrono::seconds limit)
    -> bool {
  const auto until = std::chrono::steady_clock::now() + limit;
  while (!ready()) {
    if (std::chrono::steady_clock::now() > until) return false;
    g_main_context_iteration(nullptr, FALSE);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

const char* const kProject = R"({
  "name": "Smoke",
  "canvas": {
    "cards": [{"id": "start"}, {"id": "square"}],
    "connections": [{"from": {"card": "start", "port": "out"}, "to": {"card": "square", "port": "x"}}]
  }
})";

auto Widgets() -> void {
  ui::HomeView home;
  home.ShowRecent({{"/tmp/a", "A"}, {"/tmp/b", "B"}});
  Check(home.recent().size() == 2, "Home lists the recent projects");

  const auto scene = ui::BuildScene(
      kProject,
      R"({"cards": {"start": {"x": 0, "y": 0}, "square": {"x": 0, "y": 200}}})",
      {{"start", "{}", R"({"name": "Start"})"}});
  ui::CanvasView canvas;
  canvas.Show(&scene, [](const std::string& card) {
    return card == "start" ? D::CardState::succeeded : D::CardState::running;
  });
  canvas.Select("square");

  GtkWidget* window = gtk_window_new();
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(box), home.widget());
  gtk_box_append(GTK_BOX(box), canvas.widget());
  gtk_window_set_child(GTK_WINDOW(window), box);
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);
  gtk_window_present(GTK_WINDOW(window));
  Check(WaitFor([&] { return canvas.draws() > 0; }, std::chrono::seconds(20)),
        "the Canvas is drawn on the display");
  gtk_window_destroy(GTK_WINDOW(window));

  // What is drawn: the Cards, and their place for a click.
  cairo_surface_t* surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 400, 400);
  cairo_t* cr = cairo_create(surface);
  ui::DrawScene(cr, 400, 400, scene, nullptr, "", {});
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  const auto* card = ui::CardAtPoint(400, 400, scene, 200, 100);
  Check(card && card->id == "start", "a click finds the Card drawn there");
  const auto* data = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  bool drawn = false;
  for (int y = 0; y < 400 && !drawn; ++y)
    for (int x = 0; x < 400 && !drawn; ++x)
      drawn = data[(y * stride) + (x * 4) + 3] != 0;  // alpha
  Check(drawn, "the Cards are drawn");
  cairo_surface_destroy(surface);
}

struct HostRun {
  GtkApplication* app = nullptr;
  std::shared_ptr<ui::HostLink> host;
  ui::AppWindow* window = nullptr;
  std::string project;
};

auto WithHost(HostRun& run) -> void {
  std::string error = "not answered";
  bool answered = false;
  run.host->Start([&](const std::string& e) {
    error = e;
    answered = true;
  });
  Check(WaitFor([&] { return answered; }, std::chrono::seconds(60)) &&
            error.empty(),
        "hostd starts and answers Hello: " + error);
  if (!error.empty()) return;
  ui::SetStrings(ui::Strings(run.host->language(), ui::EnglishDictionary()));

  run.window = ui::AppWindow::Create(run.app, run.host);
  gtk_window_present(run.window->window());

  // A remembered folder is on Home.
  bool remembered = false;
  H::HostRecentRememberRequestT remember;
  remember.path = run.project;
  run.host->Call<H::RecentProject>(
      std::move(remember), [&](const ui::Reply<H::RecentProject>& reply) {
        if (reply.ok()) run.project = reply.value->path()->str();
        remembered = true;
      });
  WaitFor([&] { return remembered; }, std::chrono::seconds(20));
  run.window->RefreshRecent();
  Check(WaitFor(
            [&] {
              for (const auto& entry : run.window->home().recent())
                if (entry.path == run.project) return true;
              return false;
            },
            std::chrono::seconds(20)),
        "Home lists the remembered project");

  // An Agent shows the project: hostd tells the window on Home.
  std::thread([] {
    try {
      const auto ready = tsugumi::cli::EnsureHost(
          tsugumi::cli::LaunchFromEnvironment(Env("TSUGUMI_GTK_TEST_SELF")));
      auto agent = tsugumi::cli::ConnectAgent(ready, true);
      Tsugumi::Agent::ShowRequestT show;
      show.path = Env("TSUGUMI_GTK_TEST_PROJECT");
      show.card = "start";
      show.browser = false;
      tsugumi::cli::AgentCall<Tsugumi::Agent::Shown>(*agent, std::move(show));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "ShowRequest: %s\n", e.what());
    }
  }).detach();
  Check(WaitFor(
            [&] {
              const auto& session = run.window->project().session();
              return session && session->path() == run.project;
            },
            std::chrono::seconds(30)),
        "ShowRequest opens the project in the window on Home");
  const auto session = run.window->project().session();
  if (!session) return;
  WaitFor(
      [&] { return session->phase() != ui::ProjectSession::Phase::kStarting; },
      std::chrono::seconds(240));
  Check(session->phase() == ui::ProjectSession::Phase::kOpen,
        "the project opens: " + session->message() + " " + session->detail());
  Check(
      session->scene().cards.size() == 2 && session->scene().wires.size() == 1,
      "the Canvas has the project's Cards and wire");
  Check(run.window->project().canvas().selected() == "start",
        "the Card named by ShowRequest is selected");

  // Run from the trigger: the run's and the Card's states come from
  // notifications.
  session->Run("start");
  const bool ended = WaitFor(
      [&] {
        return !session->run_id().empty() && !session->running() &&
               session->run_state() != D::RunState::idle;
      },
      std::chrono::seconds(120));
  Check(ended,
        "the run ends: " + std::string(ui::RunStateText(session->run_state())) +
            " " + session->message());
  Check(session->CardState("start") == D::CardState::succeeded,
        std::string("the trigger Card's state: ") +
            ui::CardStateText(session->CardState("start")));
}

auto Stop() -> void {
  // The test's own hostd: stop it (SIGTERM is an orderly shutdown).
  try {
    const auto ready = tsugumi::cli::EnsureHost(
        tsugumi::cli::LaunchFromEnvironment(Env("TSUGUMI_GTK_TEST_SELF")));
    if (ready.pid > 0) ::kill(static_cast<pid_t>(ready.pid), SIGTERM);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "stopping hostd: %s\n", e.what());
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  GtkApplication* app =
      gtk_application_new("dev.tsugumi.SmokeTest", G_APPLICATION_NON_UNIQUE);
  HostRun run;
  run.app = app;
  g_signal_connect(app, "activate",
                   G_CALLBACK(+[](GApplication* application, gpointer data) {
                     auto& run = *static_cast<HostRun*>(data);
                     g_application_hold(application);
                     Widgets();
                     if (!Env("TSUGUMI_HOSTD").empty()) {
                       run.project = Env("TSUGUMI_GTK_TEST_PROJECT");
                       run.host = std::make_shared<ui::HostLink>(
                           Env("TSUGUMI_GTK_TEST_SELF"));
                       WithHost(run);
                       if (run.window) gtk_window_destroy(run.window->window());
                       run.host.reset();
                       Stop();
                     }
                     g_application_release(application);
                   }),
                   &run);
  const int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  std::fprintf(stderr, "%s\n",
               failures == 0 && status == 0 ? "gtk smoke test passed"
                                            : "gtk smoke test failed");
  return failures == 0 && status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
