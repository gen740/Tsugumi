// The JSON a project daemon still writes: its journal log lines (log text
// people read) and the Nixpkgs pin handed to Nix. Its own records are
// FlatBuffers (protocol/runtime_state.fbs).
#pragma once

#include "tsugumi/util/json.hh"

namespace tsugumi::schema {
// The journal log stream: one line per event.
struct RunJournalLine {
  t::Int64<"time_ms"> time_ms;
  t::String<"phase"> phase;
  t::String<"event"> event;
  t::String<"message"> message;
};

struct CardJournalLine {
  t::Int64<"time_ms"> time_ms;
  t::String<"phase"> phase;
  t::String<"event"> event;
  t::Array<"card", v::String> card;
  t::Uint64<"invocation"> invocation;
  t::String<"message"> message;
};

// nixpkgs.json, which the Card's Nix evaluation reads.
struct NixpkgsPin {
  t::String<"url"> url;
  t::String<"narHash"> nar_hash;
};

}  // namespace tsugumi::schema
