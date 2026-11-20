#include "host/web_pages.hh"

#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <cstdlib>
#include <thread>
#include <vector>

#include "agent/wire.hh"
#include "host/failure.hh"
#include "tsugumi/util/crypto.hh"

extern char** environ;

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace wire = agent::wire;

WebPages::WebPages(TransportServer* host, TransportServer* agent, std::string user_token,
                   std::function<void(const std::string&)> publish)
    : host_(host), agent_(agent), user_token_(std::move(user_token)), publish_(std::move(publish)) {}

WebPages::~WebPages() { Stop(); }

auto WebPages::Handles(H::RequestPayload type) -> bool {
  return type == H::RequestPayload::HostWebRequest || type == H::RequestPayload::HostPageRequest;
}

auto WebPages::Stop() -> void {
  std::unique_ptr<WebEdge> edge;
  {
    std::lock_guard lock(mutex_);
    edge.swap(edge_);
  }
  if (edge) edge->stop();
}

auto WebPages::Serve(const std::filesystem::path& root, int port, bool port_chosen) -> void {
  std::lock_guard lock(mutex_);
  if (edge_) return;
  struct stat st{};
  if (root.empty() || ::stat((root / "index.html").c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    throw Failure("invalid_request", "The Web UI folder has no index.html: " + root.string());
  WebEdgeOptions options;
  options.root = root;
  options.port = port;
  options.port_chosen = port_chosen;
  options.key = crypto::random_token(32);
  options.user_token = user_token_;
  auto edge = std::make_unique<WebEdge>(std::move(options), host_, agent_);
  edge->start();
  edge_ = std::move(edge);
}

auto WebPages::Url() -> std::string {
  std::lock_guard lock(mutex_);
  return edge_ ? "http://127.0.0.1:" + std::to_string(edge_->port()) + "/" : "";
}

auto WebPages::OpenBrowser(const std::string& project) -> bool {
  std::string url;
  {
    std::lock_guard lock(mutex_);
    if (!edge_) return false;
    url = edge_->page_url(project);
  }
  // In WSL, the browser of Windows: this handler takes the URL as it is,
  // through no shell.
#ifdef __APPLE__
  std::vector<std::string> argv = {"/usr/bin/open", url};
#else
  std::vector<std::string> argv = std::getenv("WSL_DISTRO_NAME")
                                      ? std::vector<std::string>{"rundll32.exe", "url.dll,FileProtocolHandler", url}
                                      : std::vector<std::string>{"xdg-open", url};
#endif
  std::vector<char*> args;
  for (auto& a : argv) args.push_back(a.data());
  args.push_back(nullptr);
  pid_t pid = 0;
  if (::posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0) return true;
  std::thread([pid] {
    int status = 0;
    ::waitpid(pid, &status, 0);
  }).detach();
  return true;
}

auto WebPages::Show(const std::string& project, bool browser) -> Shown {
  {
    std::lock_guard lock(mutex_);
    for (const auto& [_, page] : pages_)
      if (page.project == project) return {true, false};
    for (auto& [connection, page] : pages_) {
      if (!page.project.empty() || page.detached) continue;
      page.project = project;
      H::PageShowT show;
      show.project = project;
      host_->reply(connection, wire::HostEvent(std::move(show)));
      return {true, false};
    }
  }
  return {false, browser && OpenBrowser(project)};
}

auto WebPages::Disconnected(ConnId connection) -> void {
  std::lock_guard lock(mutex_);
  pages_.erase(connection);
}

auto WebPages::Handle(const RequestIn& request, const H::Request& r) -> std::string {
  if (const auto* web = r.value_as_HostWebRequest()) {
    // The page cannot choose what this origin serves or open more pages.
    if (request.websocket)
      throw Failure("permission_denied", "The Web UI is started by a local client");
    if (web->root() && web->root()->size())
      Serve(web->root()->str(), web->port() ? static_cast<int>(web->port()) : 8780, web->port() != 0);
    H::HostWebT result;
    result.url = Url();
    if (result.url.empty()) throw Failure("unavailable", "The Web UI is not being served");
    if (web->open()) OpenBrowser(web->project() ? web->project()->str() : "");
    return wire::HostReply(request.request_id, std::move(result));
  }
  const auto* report = r.value_as_HostPageRequest();
  const auto project = report->project() ? report->project()->str() : "";
  std::string closed;
  {
    std::lock_guard lock(mutex_);
    auto& page = pages_[request.conn];
    if (!page.project.empty() && page.project != project) closed = page.project;
    page.project = project;
    page.detached = report->detached();
    if (!closed.empty())
      for (const auto& [connection, other] : pages_)
        if (!other.detached && other.project == closed) closed.clear();
  }
  // A child workflow's page closes with its project.
  if (!closed.empty()) {
    H::PageClosedT event;
    event.project = closed;
    publish_(wire::HostEvent(std::move(event)));
  }
  return wire::HostReply(request.request_id, Tsugumi::Service::AcknowledgementT{});
}

}  // namespace tsugumi::host
