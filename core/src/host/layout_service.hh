#pragma once

#include <set>
#include <stdexcept>
#include <string>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
struct LayoutFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
auto ValidateLayout(const std::string& text,
                    const std::set<std::string>& removed = {}) -> std::string;
auto LayoutHandles(Tsugumi::Host::RequestPayload type) -> bool;
auto LayoutHandle(const Tsugumi::Host::Request& request, uint64_t id)
    -> std::string;
auto VersionCanvas(const Tsugumi::Host::VersionCanvasRequest& request)
    -> Tsugumi::Host::VersionCanvasT;
}  // namespace tsugumi::host
