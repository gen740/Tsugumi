// The UI's one Host connection. hostd is started or found the way the
// tsugumi command does it (tsugumi-hostd --ensure, tsugumi/cli/launch.hh);
// the windows share this connection and each names its page on it
// (HostPageRequest), as the Mac app's windows do.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "rpc.hh"
#include "strings.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::linux_ui {

class HostLink : public std::enable_shared_from_this<HostLink> {
 public:
  using EventListener = std::function<void(const Tsugumi::Host::Event&)>;

  // executable: this program, to find tsugumi-hostd beside it.
  explicit HostLink(std::string executable);
  ~HostLink();
  HostLink(const HostLink&) = delete;
  auto operator=(const HostLink&) -> HostLink& = delete;

  // Starts or finds hostd and says Hello, off the main thread; done gets an
  // empty error when connected. The language is read from hostd's
  // app.language setting unless TSUGUMI_LANGUAGE says one.
  auto Start(std::function<void(std::string error)> done) -> void;
  [[nodiscard]] auto connected() const -> bool {
    return connection_ != nullptr;
  }
  [[nodiscard]] auto language() const -> Language { return language_; }

  template <class T, class Value>
  auto Call(Value value, std::function<void(Reply<T>)> done) -> void {
    if (!connection_) {
      Reply<T> reply;
      reply.error = L("Tsugumi のホストに接続していません");
      done(std::move(reply));
      return;
    }
    linux_ui::Call<HostProtocol, T>(*connection_, std::move(value),
                                    std::move(done));
  }

  // What a page shows: a project's folder, or "" for Home. Reported again
  // after a reconnect.
  auto ReportPage(const std::string& page, const std::string& project) -> void;
  auto ClosePage(const std::string& page) -> void;

  // Host events (PageShow, EnvironmentProgress, ...), on the main thread.
  auto Listen(EventListener listener) -> int;
  auto Unlisten(int id) -> void;
  // The connection was lost (true) or is back (false).
  std::function<void(bool lost)> on_connection;

 private:
  auto Connect(std::function<void(std::string)> done) -> void;
  auto Lost() -> void;
  auto SendPage(const std::string& page, const std::string& project,
                bool closed) -> void;

  std::string executable_;
  std::shared_ptr<client::Connection> connection_;
  Language language_ = Language::kJapanese;
  std::map<std::string, std::string> pages_;
  std::map<int, EventListener> listeners_;
  int next_listener_ = 1;
  bool reconnecting_ = false;
};

}  // namespace tsugumi::linux_ui
