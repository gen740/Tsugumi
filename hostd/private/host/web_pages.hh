// The Web UI: its origin (WebEdge) and the open pages. Pages are Host
// connections that reported what they show; other Host clients (the Mac app,
// the CLI) are not pages.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "host/web_edge.hh"
#include "tsugumi/protocol/host_generated.h"
#include "tsugumi/rpc/transport.hh"

namespace tsugumi::host {

class WebPages {
 public:
  // publish: to every Host connection. send: to one.
  WebPages(TransportServer* host, TransportServer* agent, std::string user_token,
           std::function<void(const std::string&)> publish);
  ~WebPages();

  static auto Handles(Tsugumi::Host::RequestPayload type) -> bool;
  auto Handle(const RequestIn& request, const Tsugumi::Host::Request& r) -> std::string;
  auto Disconnected(ConnId connection) -> void;
  auto Stop() -> void;

  /** Starts serving when it does not yet. Throws when the port is taken. */
  auto Serve(const std::filesystem::path& root, int port, bool port_chosen) -> void;
  /** The origin, without the key; empty while not serving. */
  auto Url() -> std::string;
  /** Opens the page in the default browser. False: not serving. */
  auto OpenBrowser(const std::string& project = {}) -> bool;

  struct Shown {
    bool shown = false;   // a page shows it now
    bool opened = false;  // a browser window was asked to open it
  };
  /** Shows a project without taking another page's away: a page that shows
   *  it already, or one on Home is told to; else a browser window opens it
   *  when browser is set. */
  auto Show(const std::string& project, bool browser) -> Shown;

 private:
  struct Page {
    std::string project;
    bool detached = false;
  };
  TransportServer* host_;
  TransportServer* agent_;
  std::string user_token_;
  std::function<void(const std::string&)> publish_;
  std::mutex mutex_;
  std::unique_ptr<WebEdge> edge_;
  std::map<ConnId, Page> pages_;
};

}  // namespace tsugumi::host
