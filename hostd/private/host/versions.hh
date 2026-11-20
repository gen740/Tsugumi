#pragma once

#include <mutex>
#include <string>

#include "tsugumi/rpc/transport.hh"

namespace tsugumi::agent {
struct Tools;
}  // namespace tsugumi::agent
namespace tsugumi::host {
struct Versions {
  explicit Versions(agent::Tools& tools);
  auto Note(const RequestIn& request, bool human,
            std::unique_lock<std::recursive_mutex>& lock) -> std::string;
  auto HandleHost(const RequestIn& request, bool human,
                  std::unique_lock<std::recursive_mutex>& lock) -> std::string;
  agent::Tools* tools;
};
}  // namespace tsugumi::host
