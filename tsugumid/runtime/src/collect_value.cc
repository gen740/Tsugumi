#include "collect_value.hh"

#include <flatbuffers/flatbuffers.h>

#include <vector>

#include "tsugumi/protocol/card_generated.h"

namespace tsugumi {

auto collect_values(const types::PortType& output,
                    std::span<const Value> members,
                    const std::atomic<bool>& cancel) -> Value {
  // tsugumi.builtin.Collected { items:[Tsugumi.Card.CardPort] (id: 0); }
  constexpr flatbuffers::voffset_t kItems = 4;
  constexpr size_t kMaxMessageBytes = size_t{16} << 20;
  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<Tsugumi::Card::CardPort>> items;
  for (const auto& member : members) {
    if (cancel.load())
      throw Failed(FailureKind::cancelled, "collect cancelled");
    const auto size = member.kind == PortKind::Path ? member.path.size()
                                                    : member.bytes.size();
    if (size > kMaxMessageBytes - builder.GetSize())
      throw Failed(FailureKind::output_invalid,
                   "collected value exceeds 16 MiB");
    const auto name = builder.CreateString(member.name);
    flatbuffers::Offset<flatbuffers::String> path;
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> bytes;
    if (member.kind == PortKind::Path) {
      path = builder.CreateString(member.path);
    } else {
      builder.ForceVectorAlignment(member.bytes.size(), sizeof(uint8_t),
                                   FLATBUFFERS_MAX_ALIGNMENT);
      bytes = builder.CreateVector(
          reinterpret_cast<const uint8_t*>(member.bytes.data()),
          member.bytes.size());
    }
    items.push_back(
        Tsugumi::Card::CreateCardPort(builder, name, member.kind, bytes, path));
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
          PortKind::FlatBuffer,
          output.type,
          {reinterpret_cast<const char*>(builder.GetBufferPointer()),
           builder.GetSize()},
          {}};
}

}  // namespace tsugumi
