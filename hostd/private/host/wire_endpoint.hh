#pragma once
#include "tsugumi/rpc/transport.hh"
namespace tsugumi {
auto AgentEndpoint(ServerOptions& options) -> void;
auto HostEndpoint(ServerOptions& options) -> void;
}  // namespace tsugumi
