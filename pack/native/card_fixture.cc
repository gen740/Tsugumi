#include <cstdlib>
#include <cstring>

#include "tsugumi/card_abi.h"
#include "tsugumi/protocol/card_generated.h"

namespace C = Tsugumi::Card;

extern "C" int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
  flatbuffers::FlatBufferBuilder builder;
  const auto name = builder.CreateString("out");
  const uint8_t marker = 42;
  const auto payload = builder.CreateVector(&marker, 1);
  const auto port = C::CreateCardPort(builder, name, {}, payload);
  builder.Finish(C::CreateCardResult(builder, builder.CreateVector(&port, 1)));
  const auto size = builder.GetSize();
  auto* data = static_cast<uint8_t*>(std::malloc(size));
  if (!data) return 1;
  std::memcpy(data, builder.GetBufferPointer(), size);
  if (call->input.size == 1) std::memset(data, 0xff, size);
  *result = {data, size};
  return 0;
}
