// Private project runtime and persistence JSON contracts.
#pragma once

#include "tsugumi/util/json.hh"

namespace tsugumi::schema {
struct StartupError {
  t::String<"kind"> kind;
  t::String<"found"> found;
  t::String<"expected"> expected;
  t::String<"source"> source;
  t::String<"message"> message;
};

struct ViewEntry {
  t::Uint64<"rev"> rev;
  t::String<"view"> view;
};

struct ViewFile {
  t::Mapping<"views", v::String, Obj<ViewEntry>> views;
};

struct UpstreamJson {
  t::String<"card"> card;
  t::String<"port"> port;
};

struct WaitingJson {
  t::String<"port"> port;
  t::Array<"upstream", Obj<UpstreamJson>> upstream;
};

struct WaitingList {
  t::Array<"waiting", Obj<WaitingJson>> items;
};

// Stage times, reuse and delivered outputs of a completed invocation
// (invocations.detail). -1 stands for "not set" (all members are required).
struct StageJson {
  t::Int64<"phase"> phase;
  t::Int64<"outcome"> outcome;
  t::Int64<"start"> start;
  t::Int64<"end"> end;
};

struct InvocationDetailJson {
  Opt<"stages", v::Array<Obj<StageJson>>> stages;
  Opt<"worker_reused", v::Int64> worker_reused;
  Opt<"outputs_sent", v::Array<v::String>> outputs_sent;
  Opt<"ready_at", v::Int64> ready_at;
  Opt<"emulator", v::String> emulator;
  Opt<"environment_id", v::String> environment_id;
  Opt<"params", v::String> params;
  Opt<"execution_service", v::String> execution_service;
  Opt<"execution_job_id", v::String> execution_job_id;
  Opt<"execution_system", v::String> execution_system;
  Opt<"execution_cpu_millis", v::Uint64> execution_cpu_millis;
  Opt<"execution_memory_bytes", v::Uint64> execution_memory_bytes;
  Opt<"execution_state", v::Int64> execution_state;
  Opt<"batch_activation", v::Uint64> batch_activation;
  Opt<"batch_index", v::Uint64> batch_index;
  Opt<"batch_size", v::Uint64> batch_size;
  Opt<"implementation_index", v::Uint64> implementation_index;
  Opt<"implementation_label", v::String> implementation_label;
};

struct InputSourceJson {
  t::String<"port"> port;
  t::String<"card"> card;
  t::Uint64<"invocation"> invocation;
};

struct InputSourceList {
  t::Array<"sources", Obj<InputSourceJson>> items;
};

struct MemberJson {
  t::String<"path"> path;
  t::Uint64<"size"> size;
  t::String<"hash"> hash;
};

struct MemberList {
  t::Array<"members", Obj<MemberJson>> items;
};

struct SavedInputJson {
  t::Int64<"kind"> kind;
  t::String<"schema"> schema;
  t::String<"root_type"> root_type;
  t::String<"data"> data_b64;
};

struct SavedInputGroupJson {
  t::String<"port"> port;
  t::Bool<"execution_list"> execution_list;
  t::Array<"values", Obj<SavedInputJson>> values;
  t::Bool<"partial_result"> partial_result;
};

struct SavedInputList {
  t::Array<"groups", Obj<SavedInputGroupJson>> groups;
};

struct BatchOutputJson {
  t::String<"port"> port;
  t::Uint64<"count"> count;
  t::Bool<"execution_list"> execution_list;
};

struct BatchOutputList {
  t::Array<"outputs", Obj<BatchOutputJson>> outputs;
};

struct CursorJson {
  t::Int64<"h"> hi;
  t::String<"a"> after;
};

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

// One Card evaluated and built ahead of its runs (tsugumid --prepare-cards);
// a core started with --prepared-cards uses it instead of evaluating.
struct PreparedCard {
  Opt<"card", v::String> card;
  Opt<"error", v::String> error;
  // The evaluation key, as the core would compute it again for this Card.
  Opt<"key", v::String> key;
  Opt<"derivation", v::String> derivation;
  Opt<"manifest", v::String> manifest;
  Opt<"system", v::String> system;
  Opt<"execution_list", v::Bool> execution_list;
  Opt<"implementations", v::Uint64> implementations;
  Opt<"implementation_labels", v::Array<v::String>> implementation_labels;
  Opt<"closure", v::Array<v::String>> closure;
};

struct NixpkgsPin {
  t::String<"url"> url;
  t::String<"narHash"> nar_hash;
};

struct SdkFileJson {
  t::String<"path"> path;
  t::String<"hash"> hash;
  t::Uint64<"size"> size;
};

struct SdkJobJson {
  t::Array<"card", v::String> card;
  t::Uint64<"rev"> rev;
  t::String<"state"> state;  // pending | ready | failed
  t::Array<"files", Obj<SdkFileJson>> files;
  t::String<"failure"> failure_b64;  // Failure FlatBuffer, empty when none
};

// ---- .tsugumi-history.json (a demo's versions,
// tsugumid/runtime/src/versions.cc) ------------

// (A struct's fields are all required or all optional: these are optional,
// and versions.cc checks the ones it needs.)
struct HistoryRun {
  Opt<"run_id", v::String> run_id;
  Opt<"state", v::String> state;
  Opt<"ended", v::String> ended;
  Opt<"seconds", Number> seconds;
};
struct HistoryCommit {
  Opt<"id", v::String> id;
  Opt<"parents", v::Array<v::String>> parents;
  Opt<"message", v::String> message;
  Opt<"time", v::Uint64> time;  // seconds since the epoch
  // What differs from the files on disk: their content, and files left out.
  Opt<"files", v::Mapping<v::String, v::String>> files;
  Opt<"removed", v::Array<v::String>> removed;
  Opt<"runs", v::Array<Obj<HistoryRun>>> runs;
  Opt<"tag", v::String> tag;
  Opt<"tagMessage", v::String> tag_message;
};
struct HistoryFile {
  t::Array<"commits", Obj<HistoryCommit>> commits;
  t::Mapping<"branches", v::String, v::String> branches;  // name -> commit id
  t::String<"head"> head;                                 // a branch name
};

// server.ready, written for the launcher into the 0700 control directory.
struct ServerReady {
  t::String<"endpoint">
      endpoint;  // ws://host:port/rpc as seen inside the environment
  t::String<"token"> token;
  t::Uint32<"protocol"> protocol;
  t::String<"project_id"> project_id;
  t::String<"unix_socket"> unix_socket;
  t::Int64<"pid"> pid;
};

}  // namespace tsugumi::schema
