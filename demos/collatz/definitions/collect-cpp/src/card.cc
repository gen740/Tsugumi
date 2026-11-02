#include <tsugumi/sdk/card.hh>

#include <string>
#include <vector>

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

// reuseWorker keeps this state for the whole run; a new run starts empty.
static std::vector<uint64_t> values;

void run(tsugumi::sdk::Context& ctx) {
  const auto* m = ctx.input<Merged>("arrival");
  if (m->value_type() == Arrival::init) values.clear();
  uint64_t n = arrival_value(m);
  values.push_back(n);
  std::string text = std::to_string(values.size()) + " values:";
  for (uint64_t v : values) text += " " + std::to_string(v);
  ctx.display_text(text);
  if (n != 1) return;
  flatbuffers::FlatBufferBuilder b;
  b.Finish(outputs::sequence::collatz::CreateSequence(b, b.CreateVector(values)));
  ctx.output("sequence", b);
}
TSUGUMI_CARD(run)
