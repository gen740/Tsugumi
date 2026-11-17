#pragma once

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
// Tools are absolute paths fixed by the host build; no PATH discovery/fallback.
auto Format(const Tsugumi::Host::FileFormatRequestT& request) -> Tsugumi::Host::FileFormatResultT;
}  // namespace tsugumi::host
