#include <tsugumi/sdk/card.hpp>

#include "_inputs_generated.h"
#include "_outputs_generated.h"

void run(tsugumi::sdk::Context& ctx) {
  uint64_t n = ctx.input<inputs::number::collatz::Number>("number")->value();
  flatbuffers::FlatBufferBuilder b;
  b.Finish(outputs::next::collatz::CreateNumber(b, n / 2));
  ctx.output("next", b);
}
TSUGUMI_CARD(run)
