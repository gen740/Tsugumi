#include "host_link.hh"

#include <glib.h>

#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

#include "tsugumi/cli/launch.hh"
#include "tsugumi/cli/rpc.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::linux_ui {
namespace H = Tsugumi::Host;

namespace {

auto ForcedLanguage() -> std::string {
  const char* forced = std::getenv("TSUGUMI_LANGUAGE");
  return forced ? forced : "";
}

// The shared settings profile of the desktop UIs (the Web UI and the Mac app
// read app.language from it too).
auto ReadLanguage(client::Connection& connection) -> Language {
  if (const auto forced = ForcedLanguage(); !forced.empty())
    return LanguageNamed(forced);
  H::SettingsGetRequestT request;
  request.profile = "desktop";
  const auto reply = cli::HostCall<H::Settings>(connection, std::move(request));
  if (reply.view->values())
    for (const auto* setting : *reply.view->values())
      if (setting->key()->str() == "app.language")
        return LanguageNamed(json::String(setting->value_json()->str()));
  return Language::kJapanese;
}

}  // namespace

HostLink::HostLink(std::string executable)
    : executable_(std::move(executable)) {}

HostLink::~HostLink() = default;

auto HostLink::Start(std::function<void(std::string error)> done) -> void {
  Connect(std::move(done));
}

auto HostLink::Connect(std::function<void(std::string)> done) -> void {
  const std::weak_ptr<HostLink> weak = weak_from_this();
  std::thread([weak, executable = executable_, done = std::move(done)] {
    std::string error;
    std::shared_ptr<client::Connection> connection;
    auto language = Language::kJapanese;
    try {
      const auto ready =
          cli::EnsureHost(cli::LaunchFromEnvironment(executable));
      client::Options options;
      options.protocol = client::Protocol::Host;
      options.endpoint = "unix:" + ready.host_socket;
      connection = std::make_shared<client::Connection>(options);
      const auto* identity = connection.get();
      connection->OnEvent([weak](client::Bytes bytes) {
        Post([weak, bytes = std::move(bytes)] {
          const auto self = weak.lock();
          if (!self) return;
          const auto* event = H::GetMessage(bytes.data())->payload_as_Event();
          if (!event) return;
          // A listener may stop listening while it is told.
          const auto listeners = self->listeners_;
          for (const auto& [id, listener] : listeners) listener(*event);
        });
      });
      connection->OnClosed([weak, identity](client::Error) {
        Post([weak, identity] {
          const auto self = weak.lock();
          if (self && self->connection_.get() == identity) self->Lost();
        });
      });
      if (auto failed = connection->Start().get())
        throw std::runtime_error(failed.message);
      Tsugumi::Service::HelloRequestT hello;
      hello.token = ready.user_token;
      cli::HostCall<Tsugumi::Service::HelloResult>(*connection,
                                                   std::move(hello));
      language = ReadLanguage(*connection);
    } catch (const std::exception& e) {
      error = e.what();
      connection.reset();
    }
    Post([weak, done, error, connection, language] {
      const auto self = weak.lock();
      if (!self) return;
      if (error.empty()) {
        self->connection_ = connection;
        self->language_ = language;
      }
      done(error);
    });
  }).detach();
}

auto HostLink::Lost() -> void {
  connection_.reset();
  if (on_connection) on_connection(true);
  if (reconnecting_) return;
  reconnecting_ = true;
  // hostd may have stopped: start or find it again, then tell it the pages.
  struct Retry {
    std::weak_ptr<HostLink> link;
  };
  g_timeout_add_seconds_full(
      G_PRIORITY_DEFAULT, 1,
      [](gpointer data) -> gboolean {
        const auto self = static_cast<Retry*>(data)->link.lock();
        if (!self) return G_SOURCE_REMOVE;
        self->Connect(
            [weak = std::weak_ptr<HostLink>(self)](const std::string& error) {
              const auto self = weak.lock();
              if (!self) return;
              self->reconnecting_ = false;
              if (!error.empty()) {
                self->Lost();
                return;
              }
              for (const auto& [page, project] : self->pages_)
                self->SendPage(page, project, false);
              if (self->on_connection) self->on_connection(false);
            });
        return G_SOURCE_REMOVE;
      },
      new Retry{weak_from_this()},
      [](gpointer data) { delete static_cast<Retry*>(data); });
}

auto HostLink::ReportPage(const std::string& page, const std::string& project)
    -> void {
  const auto known = pages_.find(page);
  if (known != pages_.end() && known->second == project) return;
  pages_[page] = project;
  SendPage(page, project, false);
}

auto HostLink::ClosePage(const std::string& page) -> void {
  if (pages_.erase(page) == 0) return;
  SendPage(page, "", true);
}

auto HostLink::SendPage(const std::string& page, const std::string& project,
                        bool closed) -> void {
  H::HostPageRequestT request;
  request.page = page;
  request.project = project;
  request.closed = closed;
  // A lost connection is reported again once it is back.
  Call<Tsugumi::Service::Acknowledgement>(
      std::move(request),
      [](const Reply<Tsugumi::Service::Acknowledgement>&) {});
}

auto HostLink::Listen(EventListener listener) -> int {
  listeners_[next_listener_] = std::move(listener);
  return next_listener_++;
}

auto HostLink::Unlisten(int id) -> void { listeners_.erase(id); }

}  // namespace tsugumi::linux_ui
