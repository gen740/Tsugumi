#pragma once

#include <map>
#include <mutex>

#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/trace_codec.hh"
#include "tsugumi/rpc/lanes.hh"
#include "tsugumi/rpc/transport.hh"

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

  explicit DebugTrace(Lane& lane) : lane_(lane) {}
  auto Attach(TransportServer& server) -> void;
  auto Reconcile(Targets targets) -> void;
  auto CatalogChanged() -> void;
  auto Disconnect(ConnId conn) -> void;
  auto Stop() -> void;
  auto Handle(ConnId conn, const Tsugumi::Trace::HostTraceRequest& request,
              const std::function<Targets()>& targets)
      -> Tsugumi::Trace::HostTraceT;

 private:
  // Callbacks run on recorder and peer transport threads. Only the trace lane
  // changes peers or subscribes a viewer.
  struct Feed {
    auto Notify(const std::string& source, bool catalog = false) -> void;

    std::mutex mutex;
    TransportServer* server = nullptr;
    ConnId viewer = 0;
    bool catalog_armed = false;
  };
  auto Subscribe(ConnId conn, bool catalog) -> void;
  struct Peer {
    Target target;
    std::unique_ptr<client::Connection> connection;
    trace::Mode mode = trace::Mode::Off;
    uint64_t generation = 0;
  };
  auto Query(Peer& peer, const std::string& project,
             std::optional<trace::Mode> mode, uint64_t after = 0)
      -> std::unique_ptr<Tsugumi::Trace::SnapshotT>;

  auto PeerClosed(const std::string& project, uint64_t generation) -> void;

  Lane& lane_;
  uint64_t next_generation_ = 0;
  std::shared_ptr<Feed> feed_ = std::make_shared<Feed>();
  std::map<std::string, Peer> peers_;
};

}  // namespace tsugumi::host
