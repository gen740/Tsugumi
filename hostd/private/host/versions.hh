#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "tsugumi/rpc/transport.hh"

namespace tsugumi::agent {
struct Tools;
struct Turns;
}  // namespace tsugumi::agent
namespace tsugumi::host {
class Settings;
struct Versions {
  Versions(Settings& settings, agent::Tools& tools, agent::Turns& turns,
           std::function<std::string(std::string)> scrub);
  auto Note(const RequestIn& request, bool human,
            std::unique_lock<std::recursive_mutex>& lock) -> std::string;
  auto HandleHost(const RequestIn& request, bool human,
                  std::unique_lock<std::recursive_mutex>& lock) -> std::string;
  Settings* host_settings;
  agent::Tools* tools;
  agent::Turns* turns;
  std::function<std::string(std::string)> scrub;
  std::map<std::pair<std::string, std::string>, std::weak_ptr<std::mutex>>
      version_notes;
};
}  // namespace tsugumi::host
