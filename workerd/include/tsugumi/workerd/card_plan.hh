// The build plan shared by the Card evaluator and execution hosts.
#pragma once

#include "tsugumi/util/json.hh"

namespace tsugumi::schema {

// Fields in the order Nix's JSON emits them (sorted), which decoding follows.
struct CardView {
  t::String<"blob"> blob;
  t::String<"name"> name;
  t::String<"type"> type;
};

struct CardPlan {
  Opt<"runs", v::Array<v::String>> runs;
  Opt<"views", v::Array<Obj<CardView>>> views;
  Opt<"worker", v::String> worker;
};

inline auto decode_card_plan(std::string_view text, CardPlan& plan)
    -> std::optional<DecodeError> {
  if (auto error = decode(text, plan)) return error;
  if (!get(plan.runs) || !get(plan.views))
    return DecodeError{"Card plan requires runs and views arrays"};
  return std::nullopt;
}

}  // namespace tsugumi::schema
