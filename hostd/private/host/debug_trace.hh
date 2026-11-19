#pragma once

#include <map>

#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/trace_codec.hh"

namespace tsugumi::host {

// All access runs on the debug lane. Keeping one authenticated connection per
// core lets the core clear capture on EOF if hostd exits or loses the link.
class DebugTrace {
 public:
  struct Target {
    std::string endpoint;
    std::string token;
    std::string capability;
    auto operator==(const Target&) const -> bool = default;
  };
  using Targets = std::map<std::string, Target>;

  auto Reconcile(Targets targets) -> void;
  auto Handle(const Tsugumi::Trace::HostTraceRequest& request,
              const std::function<Targets()>& targets)
      -> Tsugumi::Trace::HostTraceT;

 private:
  struct Peer {
    Target target;
    std::unique_ptr<client::Connection> connection;
    trace::Mode mode = trace::Mode::Off;
  };
  auto Query(Peer& peer, const std::string& project,
             std::optional<trace::Mode> mode)
      -> std::unique_ptr<Tsugumi::Trace::SnapshotT>;

  std::map<std::string, Peer> peers_;
};

}  // namespace tsugumi::host
