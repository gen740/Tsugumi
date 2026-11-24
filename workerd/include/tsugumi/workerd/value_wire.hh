// Worker value and display codecs; no project reply types.
#pragma once
#include <flatbuffers/flatbuffers.h>
#include <span>

#include "tsugumi/protocol/artifact_generated.h"

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
using ArtifactIdVector = flatbuffers::Vector<const ::tsugumi::ArtifactId*>;
auto artifact_id(const ::tsugumi::ArtifactId& id) -> ArtifactKey;
auto artifact_ids(const ArtifactIdVector* ids) -> std::vector<ArtifactKey>;
auto artifact_struct(const ArtifactKey& id) -> ::tsugumi::ArtifactId;
auto artifact_ids(FBB& b, std::span<const ArtifactKey> ids) -> Off<ArtifactIdVector>;
}  // namespace tsugumi::wire
