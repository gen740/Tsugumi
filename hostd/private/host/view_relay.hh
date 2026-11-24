#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>

#include "tsugumi/protocol/host_generated.h"
#include "tsugumi/rpc/transport.hh"

namespace tsugumi::host {

// Live state owned by the authenticated UI connection that presents a window.
// No window metadata, input or snapshot is kept after that connection ends.
class ViewRelay {
 public:
  using Deliver =
      std::function<void(ConnId, Tsugumi::Host::ViewActionDispatchT)>;

  auto Report(ConnId owner,
              const Tsugumi::Host::ViewPresentationRequest& report) -> void;
  auto Complete(ConnId owner,
                const Tsugumi::Host::ViewActionCompleteRequest& reply) -> void;
  auto Disconnect(ConnId owner) -> void;

  auto List(const std::string& project) -> Tsugumi::Host::ViewWindowsT;
  auto Execute(const Tsugumi::Host::ViewActionRequestT& request,
               const Deliver& send, std::stop_token stop)
      -> Tsugumi::Host::ViewActionResultT;

 private:
  using Address = std::pair<std::string, std::string>;
  struct Window {
    ConnId owner;
    Tsugumi::Host::ViewWindowT info;
    size_t pending = 0;
  };
  struct Pending {
    Address address;
    ConnId owner;
    bool snapshot;
    bool done = false;
    Tsugumi::Host::ViewActionResultT result;
    std::string error;
  };

  auto Close(std::map<Address, Window>::iterator window) -> void;
  std::mutex mutex_;
  std::condition_variable_any changed_;
  std::map<Address, Window> windows_;
  std::map<std::string, std::shared_ptr<Pending>> pending_;
};

}  // namespace tsugumi::host
