#pragma once

#include <functional>
#include <memory>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
class NativeTerminals {
 public:
  using Output = std::function<void(const Tsugumi::Host::HostTerminalDataT&)>;
  explicit NativeTerminals(Output output);
  ~NativeTerminals();
  auto Handle(const Tsugumi::Host::HostTerminalRequestT& request)
      -> Tsugumi::Host::HostTerminalT;
  auto Shutdown() -> void;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace tsugumi::host
