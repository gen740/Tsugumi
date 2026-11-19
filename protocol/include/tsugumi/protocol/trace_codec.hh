#pragma once

#include "tsugumi/protocol/rpc_trace_generated.h"
#include "tsugumi/rpc/trace.hh"

namespace tsugumi::trace {

// Frames are already verified by their receiving transport.
extern const Codec kFlatBufferCodec;
auto ClientName(Tsugumi::Trace::Client client) -> const char*;
auto ToWire(Snapshot snapshot) -> Tsugumi::Trace::SnapshotT;

}  // namespace tsugumi::trace
