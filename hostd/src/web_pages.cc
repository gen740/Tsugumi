#include "host/web_pages.hh"

#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

#include "agent/wire.hh"
#include "host/failure.hh"

extern char** environ;

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace wire = agent::wire;

namespace {
// Starts a helper without waiting for it (it is reaped on its own thread).
auto Spawn(std::vector<std::string> argv) -> bool {
  std::vector<char*> args;
  for (auto& a : argv) args.push_back(a.data());
  args.push_back(nullptr);
  pid_t pid = 0;
  if (::posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ) !=
      0)
    return false;
  std::thread([pid] {
    int status = 0;
    ::waitpid(pid, &status, 0);
  }).detach();
  return true;
}

auto Listing(const std::filesystem::path& dir) -> H::FolderListingT {
  namespace f = std::filesystem;
  std::error_code error;
  const auto real = f::canonical(dir, error);
  if (error || !f::is_directory(real))
    throw Failure("invalid_request", "Not a folder: " + dir.string());
  H::FolderListingT out;
  out.path = real.string();
  if (real.has_parent_path() && real.parent_path() != real)
    out.parent = real.parent_path().string();
  out.project = f::is_regular_file(real / "tsugumi.json", error);
  for (const auto& entry : f::directory_iterator(
           real, f::directory_options::skip_permission_denied, error)) {
    const auto name = entry.path().filename().string();
    std::error_code e;
    if (name.starts_with('.') || !entry.is_directory(e)) continue;
    auto item = std::make_unique<H::FolderEntryT>();
    item->name = name;
    item->project = f::is_regular_file(entry.path() / "tsugumi.json", e);
    out.entries.push_back(std::move(item));
  }
  std::sort(out.entries.begin(), out.entries.end(),
            [](const auto& a, const auto& b) { return a->name < b->name; });
  return out;
}
}  // namespace

WebPages::WebPages(TransportServer* host, TransportServer* agent,
                   std::string user_token,
                   std::function<void(const std::string&)> publish,
                   WebHome home)
    : host_(host),
      agent_(agent),
      user_token_(std::move(user_token)),
      publish_(std::move(publish)),
      home_(std::move(home)) {}

WebPages::~WebPages() { Stop(); }

auto WebPages::Handles(H::RequestPayload type) -> bool {
  return type == H::RequestPayload::HostPageRequest ||
         type == H::RequestPayload::HostHomeRequest ||
         type == H::RequestPayload::HostRevealRequest ||
         type == H::RequestPayload::HostFolderListRequest ||
         type == H::RequestPayload::HostFolderCreateRequest;
}

auto WebPages::Stop() -> void {
  std::unique_ptr<WebEdge> edge;
  {
    std::lock_guard lock(mutex_);
    edge.swap(edge_);
  }
  if (edge) edge->stop();
}

auto WebPages::Serve(const std::filesystem::path& root, int port,
                     bool port_chosen, const std::string& key) -> void {
  std::lock_guard lock(mutex_);
  if (edge_) return;
  struct stat st{};
  if (root.empty() || ::stat((root / "index.html").c_str(), &st) != 0 ||
      !S_ISREG(st.st_mode))
    throw Failure("invalid_request",
                  "The Web UI folder has no index.html: " + root.string());
  WebEdgeOptions options;
  options.root = root;
  options.port = port;
  options.port_chosen = port_chosen;
  options.key = key;
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
  std::vector<std::string> argv = {"open", url};
#else
  std::vector<std::string> argv =
      std::getenv("WSL_DISTRO_NAME")
          ? std::vector<std::string>{"rundll32.exe",
                                     "url.dll,FileProtocolHandler", url}
          : std::vector<std::string>{"xdg-open", url};
#endif
  return Spawn(std::move(argv));
}

auto WebPages::Tell(const PageId& page, const std::string& project,
                    const std::string& card) -> void {
  H::PageShowT show;
  show.project = project;
  show.card = card;
  show.page = page.second;
  host_->reply(page.first, wire::HostEvent(std::move(show)));
}

auto WebPages::Show(const std::string& project, const std::string& card,
                    bool browser) -> Shown {
  {
    std::lock_guard lock(mutex_);
    // Home: any page will do.
    if (project.empty() && !pages_.empty()) return {true, false};
    for (const auto& [id, page] : pages_)
      if (!project.empty() && page.project == project) {
        Tell(id, project, card);
        return {true, false};
      }
    for (auto& [id, page] : pages_) {
      if (project.empty() || !page.project.empty() || page.detached) continue;
      page.project = project;
      Tell(id, project, card);
      return {true, false};
    }
  }
  return {false, browser && OpenBrowser(project)};
}

auto WebPages::PageUrl(const std::string& project) -> std::string {
  std::lock_guard lock(mutex_);
  return edge_ ? edge_->page_url(project) : "";
}

auto WebPages::Disconnected(ConnId connection) -> void {
  std::lock_guard lock(mutex_);
  std::erase_if(pages_, [&](const auto& entry) {
    return entry.first.first == connection;
  });
}

auto WebPages::Handle(const RequestIn& request, const H::Request& r)
    -> std::string {
  namespace f = std::filesystem;
  if (r.value_as_HostHomeRequest()) {
    const auto* home = std::getenv("HOME");
    const auto* projects = std::getenv("TSUGUMI_PROJECTS_DIR");
    H::HostHomeT result;
    result.home = home ? home : "/";
    // Where demo copies go (TSUGUMI_PROJECTS_DIR for checks).
    result.documents =
        projects ? projects
                 : (f::path(result.home) / "Documents" / "Tsugumi").string();
    result.repo = home_.repo_root.string();
    result.vm = home_.vm;
    // /host/<name> needs bubblewrap: on a Mac, the shared VM.
#ifdef __APPLE__
    result.host_folders = home_.vm;
#else
    result.host_folders = true;
#endif
    std::error_code error;
    result.nix = home_.vm || f::exists("/nix/store", error);
    auto file = [&](const f::path& path, bool ok) {
      auto out = std::make_unique<H::RuntimeFileT>();
      out->path = path.string();
      out->ok = home_.vm || ok;
      return out;
    };
    result.tsugumid = file(home_.tsugumid, f::exists(home_.tsugumid, error));
    result.worker = file(home_.worker, f::exists(home_.worker, error));
    result.nix_dir =
        file(home_.nix_dir, f::exists(home_.nix_dir / "card-entry.nix", error));
    return wire::HostReply(request.request_id, std::move(result));
  }
  if (const auto* reveal = r.value_as_HostRevealRequest()) {
    const f::path path = reveal->path()->str();
#ifdef __APPLE__
    Spawn({"/usr/bin/open", "-R", path.string()});
#else
    Spawn({"xdg-open", path.parent_path().string()});
#endif
    return wire::HostReply(request.request_id,
                           Tsugumi::Service::AcknowledgementT{});
  }
  if (const auto* list = r.value_as_HostFolderListRequest()) {
    const auto* home = std::getenv("HOME");
    const auto path = list->path() && list->path()->size()
                          ? list->path()->str()
                          : std::string(home ? home : "/");
    return wire::HostReply(request.request_id, Listing(path));
  }
  if (const auto* create = r.value_as_HostFolderCreateRequest()) {
    auto name = create->name()->str();
    std::replace(name.begin(), name.end(), '/', '-');
    std::replace(name.begin(), name.end(), '\\', '-');
    if (name.empty() || name == "." || name == "..")
      throw Failure("invalid_request", "Invalid folder name");
    const auto dir = f::path(create->parent()->str()) / name;
    std::error_code error;
    if (!f::create_directory(dir, error))
      throw Failure("invalid_request",
                    "Cannot create " + dir.string() +
                        (error ? ": " + error.message() : ": it exists"));
    return wire::HostReply(request.request_id, Listing(dir));
  }
  const auto* report = r.value_as_HostPageRequest();
  const auto project =
      report->project() && !report->closed() ? report->project()->str() : "";
  const PageId id{request.conn, report->page() ? report->page()->str() : ""};
  std::string closed;
  {
    std::lock_guard lock(mutex_);
    const auto found = pages_.find(id);
    if (found != pages_.end() && !found->second.project.empty() &&
        found->second.project != project)
      closed = found->second.project;
    if (report->closed()) {
      if (found != pages_.end()) pages_.erase(found);
    } else {
      auto& page = pages_[id];
      page.project = project;
      page.detached = report->detached();
    }
    if (!closed.empty())
      for (const auto& [other_id, other] : pages_)
        if (!other.detached && other.project == closed) closed.clear();
  }
  // A child workflow's page closes with its project.
  if (!closed.empty()) {
    H::PageClosedT event;
    event.project = closed;
    publish_(wire::HostEvent(std::move(event)));
  }
  return wire::HostReply(request.request_id,
                         Tsugumi::Service::AcknowledgementT{});
}

}  // namespace tsugumi::host
