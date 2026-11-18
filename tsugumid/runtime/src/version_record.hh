// Language-neutral Git records. Card references are permanent IDs, never names.
#pragma once

#include "tsugumi/util/json.hh"

namespace tsugumi::version_record {
struct Commit {
  schema::t::String<"tsugumi"> format;
  schema::t::String<"kind"> kind;
  schema::t::String<"card"> card;
};
inline auto commit(const std::string& kind, const std::string& card = "") -> std::string {
  Commit record;
  *record.format = "commit";
  *record.kind = kind;
  *record.card = card;
  return schema::encode(record);
}
struct Count {
  schema::t::String<"card"> card;
  schema::t::Uint64<"count"> count;
};
struct Failure {
  schema::t::String<"card"> card;
  schema::t::String<"kind"> kind;
};
struct Run {
  schema::t::Bool<"whole"> whole;
  schema::t::String<"card"> card;
  schema::t::Bool<"propagate"> propagate;
  schema::t::Bool<"saved_inputs"> saved_inputs;
  schema::t::Array<"ran", schema::Obj<Count>> ran;
  schema::t::Array<"failures", schema::Obj<Failure>> failed;
  schema::t::Array<"never", schema::v::String> never;
};
struct RunNote {
  schema::t::String<"run_id"> run_id;
  schema::t::String<"state"> state;
  schema::t::String<"ended"> ended;
  schema::t::Float64<"seconds"> seconds;
  schema::t::String<"record_json"> record_json;
};
struct Change {
  schema::t::String<"card"> card;
  schema::t::Array<"kinds", schema::v::String> kinds;
};
struct Changes {
  schema::t::Array<"added", schema::v::String> added;
  schema::t::Array<"removed", schema::v::String> removed;
  schema::t::Array<"changed", schema::Obj<Change>> changed;
  schema::t::Bool<"workflow"> workflow;
};
}  // namespace tsugumi::version_record
