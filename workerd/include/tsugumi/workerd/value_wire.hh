// Worker value and display codecs; no project reply types.
#pragma once
#include <flatbuffers/flatbuffers.h>
#include "tsugumi/protocol/data_generated.h"
#include "tsugumi/workerd/values.hh"
namespace tsugumi::wire {
namespace D = Tsugumi::Data;
using FBB = flatbuffers::FlatBufferBuilder;
template <class T>
using Off = flatbuffers::Offset<T>;

Off<D::CardPath> path(FBB& b, const CardPath& p);
CardPath path(const D::CardPath* p);
Off<D::Failure> failure(FBB& b, const FailureData& f);
FailureData failure(const D::Failure* f);
Off<D::Port> port(FBB& b, const types::PortType& p);
Off<D::Display> display(FBB& b, const DisplayData& d);
// Throws Failed(output_invalid) when the display is malformed.
DisplayData display(const D::Display* d, std::string_view buffer);

// Standalone buffers (root = table) for persistence.
std::string standalone_display(const DisplayData& d);
std::optional<DisplayData> standalone_display(const std::string& bytes);


}  // namespace tsugumi::wire
