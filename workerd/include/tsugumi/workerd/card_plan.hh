// The build plan shared by the Card evaluator and execution hosts.
#pragma once

#include "tsugumi/util/json.hh"

namespace tsugumi::schema {

struct CardView {
  t::String<"name"> name;
  t::String<"type"> type;
  t::String<"blob"> blob;
};

struct CardPlan {
  t::Array<"runs", v::String> runs;
  t::Array<"views", Obj<CardView>> views;
  Opt<"worker", v::String> worker;
};

}  // namespace tsugumi::schema
