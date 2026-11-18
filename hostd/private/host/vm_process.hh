#pragma once
#include "host/environment.hh"
#include "tsugumi/protocol/vm_host_generated.h"

namespace tsugumi::host {
// One host policy drives the helper request and both clients' confirmation.
auto VMResetPolicy() -> Tsugumi::VMHost::ResetT;
auto DescribeVMReset(Tsugumi::Host::HostRuntimeT& runtime) -> void;

struct VMProcessOptions {
  std::filesystem::path binary, support, image;
  std::string broker_endpoint;
  uint32_t cpus = 0, memory = 0, store = 64;
  std::function<void(const Tsugumi::VMHost::ProgressT&)> progress;
  std::function<void(const Tsugumi::Host::HostRuntimeT&)> runtime;
  std::function<void(const Tsugumi::Host::HostTerminalDataT&)> terminal;
};
auto VMEnvironmentProcess(VMProcessOptions options)
    -> std::shared_ptr<EnvironmentProcess>;
auto VerifyEnvironmentEndpoint(const Tsugumi::Host::EnvironmentStateT& ready)
    -> bool;
}  // namespace tsugumi::host
