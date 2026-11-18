#include "host/events.hh"

#include <vector>

namespace tsugumi::host {
HostEvents::HostEvents(StateGate& gate, Lane& terminal_requests,
                       std::function<void(uint64_t)> close_terminal)
    : mutex(gate.mutex),
      terminal_requests(terminal_requests),
      close_terminal(std::move(close_terminal)) {}

auto HostEvents::Publish(const std::string& bytes) -> void {
  if (host_server)
    for (const auto connection : host_connections)
      host_server->reply(connection, bytes);
}

auto HostEvents::Disconnected(ConnId connection) -> void {
  std::lock_guard lock(mutex);
  principals.erase(connection);

  // Connection lifetime never cancels a core-owned conversation.
}

auto HostEvents::HostDisconnected(ConnId connection) -> void {
  std::vector<uint64_t> sessions;
  {
    std::lock_guard lock(mutex);
    host_connections.erase(connection);
    for (auto it = terminals.begin(); it != terminals.end();) {
      if (it->second == connection) {
        sessions.push_back(it->first);
        it = terminals.erase(it);
      } else
        ++it;
    }
  }
  for (const auto session : sessions)
    terminal_requests.submit([this, session]() {
      try {
        close_terminal(session);
      } catch (...) {
      }
    });
}

}  // namespace tsugumi::host
