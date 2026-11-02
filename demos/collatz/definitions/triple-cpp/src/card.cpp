#include <tsugumi/sdk/card.hpp>

#include <limits>
#include <string>

#include "_inputs_generated.h"
#include "_outputs_generated.h"

void run(tsugumi::sdk::Context& ctx) {
  uint64_t n = ctx.input<inputs::number::collatz::Number>("number")->value();
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  if (n > (kMax - 1) / 3)
    throw tsugumi::sdk::Failure("output_invalid", "integer overflow: 3*" + std::to_string(n) + "+1 exceeds uint64");
  flatbuffers::FlatBufferBuilder b;
  b.Finish(outputs::next::collatz::CreateNumber(b, 3 * n + 1));
  ctx.output("next", b);
}
TSUGUMI_CARD(run)
