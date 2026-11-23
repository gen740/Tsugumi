#include "collect_value.hh"

#include <flatbuffers/flatbuffers.h>

#include <set>
#include <vector>

#include "tsugumi/protocol/card_generated.h"
#include "tsugumi/workerd/value_wire.hh"

namespace tsugumi {

auto collect_values(const types::PortType& output,
                    std::span<const Value> members,
                    const std::atomic<bool>& cancel) -> Value {
  // tsugumi.builtin.Collected { items:[Tsugumi.Card.CardPort] (id: 0); }
  constexpr flatbuffers::voffset_t kItems = 4;
  constexpr size_t kMaxMessageBytes = size_t{16} << 20;
  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<Tsugumi::Card::CardPort>> items;
  std::set<ArtifactKey> artifacts;
  for (const auto& member : members) {
    if (cancel.load())
      throw Failed(FailureKind::cancelled, "collect cancelled");
    if (member.bytes.size() > kMaxMessageBytes - builder.GetSize())
      throw Failed(FailureKind::output_invalid,
                   "collected value exceeds 16 MiB");
    builder.ForceVectorAlignment(member.bytes.size(), sizeof(uint8_t),
                                 FLATBUFFERS_MAX_ALIGNMENT);
    const auto bytes = builder.CreateVector(
        reinterpret_cast<const uint8_t*>(member.bytes.data()), member.bytes.size());
    items.push_back(Tsugumi::Card::CreateCardPort(
        builder, builder.CreateString(member.name), builder.CreateString(member.schema),
        bytes, wire::artifact_ids(builder, member.artifacts)));
    artifacts.insert(member.artifacts.begin(), member.artifacts.end());
    if (builder.GetSize() > kMaxMessageBytes)
      throw Failed(FailureKind::output_invalid,
                   "collected value exceeds 16 MiB");
  }
  const auto vector = builder.CreateVector(items);
  const auto start = builder.StartTable();
  builder.AddOffset(kItems, vector);
  builder.Finish(
      flatbuffers::Offset<flatbuffers::Table>(builder.EndTable(start)));
  if (cancel.load()) throw Failed(FailureKind::cancelled, "collect cancelled");
  if (builder.GetSize() > kMaxMessageBytes)
    throw Failed(FailureKind::output_invalid, "collected value exceeds 16 MiB");
  return {output.name,
          output.type,
          {reinterpret_cast<const char*>(builder.GetBufferPointer()),
           builder.GetSize()},
          {}, {artifacts.begin(), artifacts.end()}};
}

}  // namespace tsugumi
