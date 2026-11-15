#include "host/layout_observer.hh"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

#include "agent/catalog.hh"
#include "agent/json.hh"
#include "host/layout_saving.hh"
#include "host/save.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "util/crypto.hh"

namespace tsugumi::host {
namespace json = agent::json;
struct LayoutObserver::Watcher {
  std::string endpoint, token;
  std::atomic<bool> active{true};
  client::Connection connection;
  std::mutex mutex;
  std::condition_variable_any changed;
  bool dirty = true;
  std::jthread thread;
  Watcher(const std::string& project, const ProjectRegistry::Project& remote,
          const Call& call)
      : endpoint(remote.endpoint), token(remote.token), connection([&] {
          client::Options options;
          options.endpoint = remote.endpoint;
          return options;
        }()) {
    connection.OnEvent([this](client::Bytes bytes) {
      const auto* notification =
          Tsugumi::RPC::GetMessage(bytes.data())->payload_as_Notification();
      if (!notification || notification->value_type() !=
                               Tsugumi::RPC::NotificationPayload::DocPatched)
        return;
      std::lock_guard lock(mutex);
      dirty = true;
      changed.notify_all();
    });
    connection.OnClosed([this](client::Error) { active = false; });
    thread = std::jthread([this, project, remote, call](std::stop_token stop) {
      try {
        const agent::Catalog catalog;
        if (connection.Start().get()) return;
        auto request = [&](const std::string& name, const std::string& args) {
          return connection
              .Call([&](uint64_t id) {
                const auto bytes = agent::EncodeProjectRequest(name, args, id);
                return client::Bytes(bytes.begin(), bytes.end());
              })
              .get();
        };
        auto hello = request("session_hello",
                             json::Encode(json::Members{
                                 {"protocol", "1"},
                                 {"client_id", json::Quote(crypto::uuid4())},
                                 {"expected_project_id", json::Quote(project)},
                                 {"token", json::Quote(remote.token)}}));
        if (hello.error) return;
        if (request("project_open", "{}").error) return;
        while (!stop.stop_requested()) {
          {
            std::unique_lock lock(mutex);
            if (!changed.wait(lock, stop, [&] { return dirty; })) break;
            dirty = false;
          }
          try {
            const auto reply =
                json::Object(call(project, "project_inspect", "{}"));
            if (json::Text(reply, "reply") != "ProjectSnapshot") continue;
            const auto snapshot = json::Object(json::Get(reply, "value", "{}"));
            const auto base = CaptureSaveBase(snapshot, [&](const auto& method,
                                                            const auto& query) {
              const auto reply =
                  json::Object(call(project, method, json::Encode(query)));
              if (json::Text(reply, "reply") == "Failure")
                throw std::runtime_error("Project changed during placement");
              return json::Object(json::Get(reply, "value", "{}"));
            });
            std::map<std::string, LayoutFile> files;
            for (const auto& [path, text] : base.files)
              files[path] = {text, {}};
            for (const auto& raw :
                 json::Array(json::Get(snapshot, "sources", "[]"))) {
              const auto source = json::Object(raw);
              const auto path = json::Text(source, "path");
              if (files.contains(path))
                files[path].hash = json::Text(source, "hash");
            }
            std::vector<std::string> changes;
            for (const auto& [path, file] : files) {
              if (path != "tsugumi.json" &&
                  !IsExternalDocument(path, "tsugumi.json"))
                continue;
              const auto prefix = path.substr(0, path.size() - 12);
              const auto lp = prefix + "layout.json";
              const auto old = files.contains(lp) ? files.at(lp).text : "{}";
              const auto placed = CompleteLayout(
                  file.text, old,
                  [&](const std::string& path) -> std::optional<LayoutFile> {
                    const auto found = files.find(prefix + path);
                    return found == files.end()
                               ? std::nullopt
                               : std::optional<LayoutFile>{found->second};
                  });
              if (placed == old) continue;
              json::Members value;
              if (prefix.empty())
                value["json"] = json::Quote(placed);
              else {
                value = {{"path", json::Quote(lp)},
                         {"bytes", json::Quote(placed)}};
                if (files.contains(lp) && files.at(lp).hash)
                  value["base_hash"] = json::Quote(*files.at(lp).hash);
              }
              changes.push_back(json::Encode(json::Members{
                  {"value_type",
                   json::Quote(prefix.empty() ? "SetLayout" : "PutSource")},
                  {"value", json::Encode(value)}}));
            }
            if (changes.empty()) continue;
            const auto result =
                json::Object(call(project, "project_save",
                                  json::Encode(json::Members{
                                      {"base_rev", json::Get(snapshot, "rev")},
                                      {"op_id", json::Quote(crypto::uuid4())},
                                      {"origin", json::Quote("host-layout")},
                                      {"changes", json::Encode(changes)}})));
            if (json::Text(result, "reply") == "Failure") {
              const auto failure =
                  json::Object(json::Get(result, "value", "{}"));
              if (json::Text(failure, "kind") == "revision_conflict") {
                std::lock_guard lock(mutex);
                dirty = true;
              }
            }
          } catch (const std::exception& error) {
            if (!stop.stop_requested())
              std::cerr << "Host placement for " << project << ": "
                        << error.what() << '\n';
          }
        }
      } catch (const std::exception& error) {
        if (!stop.stop_requested())
          std::cerr << "Host placement subscription for " << project << ": "
                    << error.what() << '\n';
      }
      active = false;
    });
  }
  ~Watcher() {
    thread.request_stop();
    changed.notify_all();
    connection.Close();
    if (thread.joinable()) thread.join();
  }
};
LayoutObserver::LayoutObserver(Call call) : call_(std::move(call)) {}
LayoutObserver::~LayoutObserver() { Stop(); }
auto LayoutObserver::Watch(const std::string& project,
                           const ProjectRegistry::Project& endpoint) -> void {
  std::lock_guard lock(mutex_);
  if (stopped_) return;
  const auto it = watchers_.find(project);
  if (it != watchers_.end() && it->second->active &&
      it->second->endpoint == endpoint.endpoint &&
      it->second->token == endpoint.token)
    return;
  if (it != watchers_.end()) {
    it->second->thread.request_stop();
    it->second->changed.notify_all();
    it->second->connection.Close();
    // Registration holds the host state gate. Join only after releasing it.
    retired_.push_back(std::move(it->second));
  }
  watchers_[project] = std::make_unique<Watcher>(project, endpoint, call_);
}
auto LayoutObserver::Stop() -> void {
  decltype(watchers_) watchers;
  decltype(retired_) retired;
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    watchers.swap(watchers_);
    retired.swap(retired_);
  }
}
}  // namespace tsugumi::host
