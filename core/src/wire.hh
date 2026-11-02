// FlatBuffers encoding/decoding between native data and the RPC schema.
#pragma once

#include <flatbuffers/flatbuffers.h>

#include <string>

#include "graph.hh"
#include "model.hh"
#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi::wire {

namespace R = Tsugumi::RPC;
namespace D = Tsugumi::Data;
using FBB = flatbuffers::FlatBufferBuilder;
template <class T>
using Off = flatbuffers::Offset<T>;

Off<D::CardPath> path(FBB& b, const CardPath& p);
CardPath path(const D::CardPath* p);
// Validates a CardPath from the wire: non-empty, valid ids. Throws Failed.
CardPath card_arg(const D::CardPath* p, const char* field = "card");

Off<D::Failure> failure(FBB& b, const FailureData& f);
FailureData failure(const D::Failure* f);
Off<D::Diagnostic> diagnostic(FBB& b, const Diag& d);
Off<flatbuffers::Vector<Off<D::Diagnostic>>> diagnostics(FBB& b, const std::vector<Diag>& d);
Off<D::Port> port(FBB& b, const types::PortType& p);
Off<R::ResolvedCard> resolved(FBB& b, const Node& n);
Off<flatbuffers::Vector<Off<R::ResolvedCard>>> resolution(FBB& b, const Resolution& r);
Off<R::SourceInfo> source_info(FBB& b, const std::string& path, D::SourceKind kind, bool exists,
                               const std::optional<std::string>& hash,
                               std::optional<uint64_t> size);
Off<R::Definition> definition(FBB& b, const std::string& id, const DefinitionDoc& d);
Off<R::RunStatus> run_status(FBB& b, const RunStatusData& s);
Off<R::RunSummary> run_summary(FBB& b, const RunSummaryData& s);
Off<R::InvocationSummary> invocation(FBB& b, const InvocationSummaryData& s);
Off<D::Display> display(FBB& b, const DisplayData& d);
// Throws Failed(output_invalid) when the display is malformed.
DisplayData display(const D::Display* d);

// Finishes a reply Message.
std::string reply(FBB& b, uint64_t request_id, R::ReplyPayload type, Off<void> value);
std::string failure_reply(uint64_t request_id, const FailureData& f);

// Standalone buffers (root = table) for persistence.
std::string standalone_display(const DisplayData& d);
std::optional<DisplayData> standalone_display(const std::string& bytes);

// Reflection based deep copy of a table of the RPC schema into `b`.
Off<void> copy_table(FBB& b, const char* type_name, const flatbuffers::Table* table);

}  // namespace tsugumi::wire
