#include <tsugumi/sdk/card.hh>

#include "_inputs_generated.h"
#include "_outputs_generated.h"

// The loop delivers a merge union tagged with the arrival port.
using Merged = inputs::arrival::tsugumi::merge::Merged;
using Arrival = inputs::arrival::tsugumi::merge::Arrival;

static uint64_t arrival_value(const Merged* m) {
  switch (m->value_type()) {
    case Arrival::init: return m->value_as_init()->value();
    case Arrival::half: return m->value_as_half()->value();
    case Arrival::triple: return m->value_as_triple()->value();
    default: throw tsugumi::sdk::Failure("output_invalid", "unexpected arrival");
  }
}

// Emits nothing at 1 (the loop ends); otherwise the value and its parity.
void run(tsugumi::sdk::Context& ctx) {
  uint64_t n = arrival_value(ctx.input<Merged>("arrival"));
  if (n == 0) throw tsugumi::sdk::Failure("output_invalid", "Collatz is defined for positive integers");
  if (n == 1) return;
  {
    flatbuffers::FlatBufferBuilder b;
    b.Finish(outputs::value::collatz::CreateNumber(b, n));
    ctx.output("value", b);
  }
  flatbuffers::FlatBufferBuilder b;
  b.Finish(outputs::even::collatz::CreateParity(b, n % 2 == 0));
  ctx.output("even", b);
}
TSUGUMI_CARD(run)
