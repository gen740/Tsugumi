#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "host/state_gate.hh"
#include "lanes.hh"
#include "server.hh"

namespace tsugumi::host {
struct HostEvents {
  HostEvents(StateGate& gate, Lane& terminal_requests,
             std::function<void(uint64_t)> close_terminal);
  auto Publish(const std::string& bytes) -> void;
  auto Disconnected(ConnId connection) -> void;
  auto HostDisconnected(ConnId connection) -> void;

  std::recursive_mutex& mutex;
  Lane& terminal_requests;
  std::function<void(uint64_t)> close_terminal;
  Server* host_server = nullptr;
  std::set<ConnId> host_connections;
  std::map<uint64_t, ConnId> terminals;
  std::map<ConnId, bool> principals;
};
}  // namespace tsugumi::host
