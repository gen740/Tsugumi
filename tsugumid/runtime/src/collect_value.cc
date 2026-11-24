#include "collect_value.hh"

#include <flatbuffers/flatbuffers.h>

#include <vector>

#include "tsugumi/document/schema_id.hh"
#include "tsugumi/protocol/card_generated.h"

namespace tsugumi {

auto collect_values(const types::PortType& output,
                    std::span<const Value> members,
                    const std::atomic<bool>& cancel) -> Value {
  constexpr size_t kMaxMessageBytes = size_t{16} << 20;
  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<builtin::Envelope>> items;
  for (const auto& member : members) {
    if (cancel.load())
      throw Failed(FailureKind::cancelled, "collect cancelled");
    if (member.bytes.size() > kMaxMessageBytes - builder.GetSize())
      throw Failed(FailureKind::output_invalid,
                   "collected value exceeds 16 MiB");
    const auto schema = builder.CreateString(member.schema);
    builder.ForceVectorAlignment(member.bytes.size(), sizeof(uint8_t),
                                 FLATBUFFERS_MAX_ALIGNMENT);
    const auto bytes = builder.CreateVector(
        reinterpret_cast<const uint8_t*>(member.bytes.data()),
        member.bytes.size());
    items.push_back(builtin::CreateEnvelope(builder, schema, bytes));
    if (builder.GetSize() > kMaxMessageBytes)
      throw Failed(FailureKind::output_invalid,
                   "collected value exceeds 16 MiB");
  }
  builder.Finish(builtin::CreateCollected(builder, builder.CreateVector(items)));
  if (cancel.load()) throw Failed(FailureKind::cancelled, "collect cancelled");
  if (builder.GetSize() > kMaxMessageBytes)
    throw Failed(FailureKind::output_invalid, "collected value exceeds 16 MiB");
  return {output.name,
          output.type,
          {reinterpret_cast<const char*>(builder.GetBufferPointer()),
           builder.GetSize()},
          schema_id(*output.type)};
}

}  // namespace tsugumi
