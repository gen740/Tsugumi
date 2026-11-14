// Typed JSON schemas (hosq) for every JSON document the core reads or writes.
//
// Reading: the whole input is validated first with json::GenericDom (UTF-8,
// grammar, duplicate keys anywhere, non-finite numbers), then the validated
// root is decoded into the typed schema. Unknown members are rejected.
// Writing: schemas whose fields are all required are encoded with the compact
// JSON writer. Document files (tsugumi.json, layout.json, _card.json,
// _meta.json) are not re-encoded: their confirmed bytes are the validated client
// text, which also keeps the original integer spellings. One exception: a save
// that renames or deletes a Card rewrites layout.json when it still names that
// Card (document::layout_following_cards), keeping every value's spelling.
#pragma once

#include <hosq/hosq.hh>
#include <hosq/json/compact_writer.hh>
#include <hosq/json/dom.hh>
#include <hosq/json/fast_reader.hh>

#include <optional>
#include <type_traits>
#include <string>
#include <string_view>
#include <vector>

namespace tsugumi::schema {

namespace t = hosq::types;
namespace v = hosq::types::value;

// Captured arbitrary JSON value (validated, owned, original spelling kept).
using Json = hosq::Elem<hosq::decode::json_dom<> | hosq::store::assign, hosq::encode::json_dom>;
// A number accepted from integer or decimal spellings (layout coordinates).
using Number = hosq::Elem<hosq::scan::scalar | hosq::decode::number<double> | hosq::store::assign,
                          hosq::encode::value>;
template <class S>
using Obj = hosq::Elem<hosq::scan::object | hosq::decode::to<S> | hosq::store::assign, hosq::encode::value>;
template <hosq::StringLiteral K, class E>
using Opt = t::Optional<K, E>;

// ---- tsugumi.json ----------------------------------------------------------

struct StartupError {
  t::String<"kind"> kind;
  t::String<"found"> found;
  t::String<"expected"> expected;
  t::String<"source"> source;
  t::String<"message"> message;
};

struct Endpoint {
  Opt<"card", v::String> card;
  Opt<"port", v::String> port;
};

struct Connection {
  Opt<"from", Obj<Endpoint>> from;
  Opt<"to", Obj<Endpoint>> to;
};

struct Boundary {
  Opt<"inputs", v::Mapping<v::String, Obj<Endpoint>>> inputs;
  Opt<"outputs", v::Mapping<v::String, Obj<Endpoint>>> outputs;
};

// A Card on the canvas. Its id is its name and the folder of its own
// definition (definitions/<id>/), which holds every setting.
struct CardInstance {
  Opt<"id", v::String> id;
};

struct Graph {
  Opt<"cards", v::Array<Obj<CardInstance>>> cards;
  Opt<"connections", v::Array<Obj<Connection>>> connections;
};

// A Nix source outside nixpkgs that this project opts in to. Its Cards read it
// as inputs.sources.<name>. `type` says how it is read: "flake" for now, whose
// `ref` is locked (a commit and its narHash).
struct ExternalPackageOverride {
  Opt<"override", v::Mapping<v::String, Json>> arguments;
  Opt<"overrideAttrs", v::Mapping<v::String, Json>> attributes;
};

struct NixSource {
  Opt<"name", v::String> name;
  Opt<"type", v::String> type;
  Opt<"ref", v::String> ref;
  Opt<"overrides", Json> overrides;
};

struct Project {
  Opt<"name", v::String> name;
  Opt<"pkgsPath", v::String> pkgs_path;
  Opt<"nixSources", v::Array<Obj<NixSource>>> nix_sources;
  // Finished runs are also kept in the project (.tsugumi/runs.sqlite); default true.
  Opt<"saveRunHistory", v::Bool> save_run_history;
  Opt<"canvas", Obj<Graph>> canvas;
  // The ports this project has as a Card in Card (an external of another
  // project): outer port name -> a port of one of its Cards.
  Opt<"boundary", Obj<Boundary>> boundary;
};

// ---- definitions/<id>/_card.json ---------------------------------------------

struct ParamDef {
  Opt<"name", v::String> name;
  Opt<"type", v::String> type;
  Opt<"data", Json> data;
  Opt<"nix", v::Bool> nix;
};

struct InputDef {
  Opt<"name", v::String> name;
  Opt<"optional", v::Bool> optional;
};

// A folder the worker may read (Experimental): a folder of the Mac, seen in
// the worker as /host/<name>.
struct HostFolder {
  Opt<"path", v::String> path;  // absolute path on the Mac
  Opt<"name", v::String> name;  // folder name under /host (an id)
  Opt<"write", v::Bool> write;  // default false: read-only
};

struct ExecutionResourcesJson {
  Opt<"cpuMillis", v::Uint64> cpu_millis;
  Opt<"memoryBytes", v::Uint64> memory_bytes;
};

struct CardDefinition {
  Opt<"symbol", v::String> symbol;
  Opt<"language", v::String> language;
  Opt<"template", v::String> template_name;
  Opt<"params", v::Array<Obj<ParamDef>>> params;
  Opt<"inputs", v::Array<Obj<InputDef>>> inputs;
  // When the Card runs: "allInputs" (every required input has a value, the
  // default) or "eachInput" (once per arriving value).
  Opt<"trigger", v::String> trigger;
  Opt<"maxRuns", v::Uint64> max_runs;
  Opt<"timeoutSeconds", Json> timeout_seconds;            // positive integer or null
  Opt<"buildTimeoutSeconds", Json> build_timeout_seconds;  // positive integer or null
  Opt<"recordIO", v::Bool> record_io;
  Opt<"retainArtifact", v::Bool> retain_artifact;
  Opt<"artifactOutput", v::String> artifact_output;
  Opt<"filesystem", v::Bool> filesystem;
  Opt<"sandbox", v::Bool> sandbox;
  Opt<"reuseWorker", v::Bool> reuse_worker;  // Keep worker state across invocations and runs. Default false.
  Opt<"network", v::Bool> network;           // Experimental, default false
  Opt<"executionService", v::String> execution_service;
  Opt<"executionResources", Obj<ExecutionResourcesJson>> execution_resources;
  // The Nix system the Card is built for and runs as: "x86_64-linux" or
  // "aarch64-linux". Default: this machine's. Another one runs emulated.
  Opt<"system", v::String> system;
  // How a figure on the canvas follows the Card's results: "update" (the
  // default: the last figure, dimmed while the Card or one upstream runs) or
  // "live" (each revision of a running Card replaces the last one in place).
  // The clients read it; core only checks the value.
  Opt<"displayMode", v::String> display_mode;
  Opt<"hostFolders", v::Array<Obj<HostFolder>>> host_folders;  // Experimental
};

// definitions/<id>/_link.json: the Card is a Card in Card, the project
// externals/<external>/ (never a symlink).
struct CardLink {
  Opt<"external", v::String> external;
};

// Purpose of a port in words (Drafts have no types yet).
struct PortNote {
  Opt<"name", v::String> name;
  Opt<"description", v::String> description;
};

// What the user and the Agent write about a Card already written. author,
// editedBy, stateBy ("agent" or "user") and cards are the record of who did what;
// a model's save has them stamped (comments.hh).
struct CardComment {
  Opt<"id", v::String> id;
  Opt<"text", v::String> text;
  Opt<"resolved", v::Bool> resolved;
  Opt<"resolution", v::String> resolution;  // one line on what was done about it
  Opt<"author", v::String> author;
  Opt<"editedBy", v::String> edited_by;
  Opt<"stateBy", v::String> state_by;
  Opt<"cards", v::Array<v::String>> cards;  // what the save that resolved it changed
};

// _meta.json: the intent of a definition, for every kind of Card (a Card in
// Card too); kept when a Draft is implemented. The summary is only here.
struct CardMeta {
  Opt<"description", v::String> description;  // short summary
  Opt<"content", v::String> content;          // a Draft's plan: inputs, outputs, processing (free text)
  Opt<"context", v::String> context;          // extra intent/notes for the Agent and readers
  Opt<"origin", v::String> origin;            // where an imported definition came from
  Opt<"inputs", v::Array<Obj<PortNote>>> inputs;
  Opt<"outputs", v::Array<Obj<PortNote>>> outputs;
  Opt<"comments", v::Array<Obj<CardComment>>> comments;
};

// ---- layout.json -------------------------------------------------------------

struct Position {
  Opt<"x", Number> x;
  Opt<"y", Number> y;
  Opt<"collapsed", v::Bool> collapsed;  // display only; the Card still runs
  Opt<"inputOrder", v::Array<v::String>> input_order;
  Opt<"outputOrder", v::Array<v::String>> output_order;  // visual order by name; connections stay unchanged
};

struct StickyNote {
  Opt<"id", v::String> id;
  Opt<"scope", v::String> scope;  // graph scope path key ("" = root)
  Opt<"text", v::String> text;    // Markdown
  Opt<"color", v::String> color;
  Opt<"x", Number> x;
  Opt<"y", Number> y;
  Opt<"width", Number> width;
  Opt<"height", Number> height;
  Opt<"fontSize", Number> font_size;  // text size of the note (default 13)
};

struct DisplayPanel {
  Opt<"x", Number> x;
  Opt<"y", Number> y;
  Opt<"width", Number> width;
  Opt<"height", Number> height;
  Opt<"hidden", v::Bool> hidden;  // frame removed from the canvas; the Card is unchanged
};

struct GroupPort {
  Opt<"card", v::String> card;
  Opt<"port", v::String> port;
};

// Visual grouping only: never changes execution order or the graph.
struct CardGroup {
  Opt<"id", v::String> id;
  Opt<"scope", v::String> scope;
  Opt<"name", v::String> name;
  Opt<"color", v::String> color;
  Opt<"cards", v::Array<v::String>> cards;              // instance ids within scope
  Opt<"outputs", v::Array<Obj<GroupPort>>> outputs;     // outputs shown on the group frame
};

// Where the inner canvas of a Card in Card shows its boundary bars ("This Card's
// input" / "output"); placed automatically when absent.
struct BoundaryLayout {
  Opt<"inputs", Obj<Position>> inputs;
  Opt<"outputs", Obj<Position>> outputs;
};

struct Layout {
  // The direction the Cards were laid out in: "topToBottom" or "leftToRight".
  Opt<"flow", v::String> flow;
  Opt<"cards", v::Mapping<v::String, Obj<Position>>> cards;  // card path key -> position
  Opt<"boundaries", v::Mapping<v::String, Obj<BoundaryLayout>>> boundaries;  // scope path key -> bars
  Opt<"boundary", Obj<BoundaryLayout>> boundary;  // a Card in Card's own layout: its boundary bars
  Opt<"notes", v::Array<Obj<StickyNote>>> notes;
  Opt<"displays", v::Mapping<v::String, Obj<DisplayPanel>>> displays;  // card path key -> panel
  Opt<"groups", v::Array<Obj<CardGroup>>> groups;
};

// ---- core internal files (all fields required: encodable) ----------------------

struct JournalFile {
  t::String<"p"> path;
  t::Bool<"delete"> remove;
  t::String<"c"> content_b64;
};

struct JournalRecord {
  t::String<"t"> type;  // "txn" | "commit" | "receipt"
  t::Uint64<"rev"> rev;
  t::Uint64<"execution_rev"> execution_rev;
  t::String<"op"> op;
  t::String<"kind"> kind;
  t::String<"digest"> digest;
  t::String<"receipt"> receipt_b64;
  t::Array<"files", Obj<JournalFile>> files;
};

struct ViewCamera {
  t::Array<"scope", v::String> scope;
  t::Float64<"x"> x;
  t::Float64<"y"> y;
  t::Float64<"zoom"> zoom;
};

struct ViewStateJson {
  t::Array<"scope", v::String> scope;
  t::Array<"cameras", Obj<ViewCamera>> cameras;
  t::Array<"selected_cards", v::Array<v::String>> selected_cards;
};

struct ViewEntry {
  t::Uint64<"rev"> rev;
  t::Field<"view", Obj<ViewStateJson>> view;
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
  t::String<"d"> digest;
  t::Int64<"h"> hi;
  t::String<"a"> after;
  t::Int64<"e"> expires;
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

struct NixpkgsPin {
  t::String<"url"> url;
  t::String<"narHash"> nar_hash;
};

struct BuildCache {
  t::String<"out"> out;
};

// The manifest written by the SDK's tsugumi.manifest (read only). `run` is the
// Card body: a package (store path; the worker loads lib/main.so or the SDK-only lib/runtime.so in it) or a
// bash script, never both.
struct ManifestRun {
  Opt<"package", v::String> package;
  Opt<"bash", v::String> bash;
  Opt<"worker", v::String> worker;  // a package of another system: the worker built for it
};

struct CardManifest {
  Opt<"run", Obj<ManifestRun>> run;
  Opt<"runs", v::Array<Obj<ManifestRun>>> runs;
  Opt<"shell", v::String> shell;
  Opt<"path", v::String> path;
  Opt<"preRun", v::String> pre_run;
  Opt<"postRun", v::String> post_run;
  Opt<"views", v::String> views;  // JSON: view module name -> Nix store directory
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

// ---- .tsugumi-history.json (a demo's versions, core/src/versions.cc) ------------


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
  t::String<"head"> head;                                  // a branch name
};

// attached.json in the control folder: the folders of this host attached to
// a chat, written by the launcher (a tsugumid on the host reads only these).
struct Attached {
  t::Array<"folders", v::String> folders;
};

// server.ready, written for the launcher into the 0700 control directory.
struct ServerReady {
  t::String<"endpoint"> endpoint;  // ws://host:port/rpc as seen inside the environment
  t::String<"token"> token;
  t::Uint32<"protocol"> protocol;
  t::String<"project_id"> project_id;
  t::String<"server_instance_id"> server_instance_id;
  t::String<"unix_socket"> unix_socket;
  t::Int64<"pid"> pid;
};

// ---- helpers ---------------------------------------------------------------------

struct DecodeError {
  std::string message;
  uint64_t offset = 0;
};

namespace detail {
std::optional<DecodeError> validate(std::string_view text, hosq::json::GenericDom<>& dom);
std::string describe(const hosq::Error& e);
}  // namespace detail

// Strict decode of a complete JSON document into schema S.
template <class S>
std::optional<DecodeError> decode(std::string_view text, S& out) {
  hosq::json::GenericDom<> dom;
  if (auto err = detail::validate(text, dom)) return err;
  auto e = dom[""].decode_into(out);
  if (e) return DecodeError{detail::describe(e), e.offset};
  return std::nullopt;
}

// Decodes a captured JSON value into schema/Elem S.
template <class S>
std::optional<DecodeError> decode(const hosq::json::GenericDom<>& value, S& out) {
  auto e = value[""].decode_into(out);
  if (e) return DecodeError{detail::describe(e), e.offset};
  return std::nullopt;
}

template <class S>
std::string encode(const S& value) {
  std::string out;
  hosq::json::CompactWriter<> w{out};
  if (!hosq::encode_into(value, w)) throw std::runtime_error("JSON encoding failed");
  return out;
}

// Source text of a captured value (validated JSON, original spelling).
std::string text(const hosq::json::GenericDom<>& value);
// Captures a JSON text as a value. Throws std::runtime_error when invalid.
hosq::json::GenericDom<> capture(std::string_view json_text);
// Captures a string value.
hosq::json::GenericDom<> string_value(const std::string& s);
// JSON object text from (key, captured value) pairs, in the given order.
std::string object_text(const std::vector<std::pair<std::string, const hosq::json::GenericDom<>*>>& members);
// The form the clients write document files in: two spaces, a final newline.
// `compact` is validated JSON without whitespace between its tokens.
std::string pretty_text(std::string_view compact);
bool is_null(const hosq::json::GenericDom<>& value);

// Value of an optional field.
template <class F>
auto get(const F& field) {
  using T = std::remove_cvref_t<decltype(field->value())>;
  return field->has_value() ? &field->value() : static_cast<const T*>(nullptr);
}

}  // namespace tsugumi::schema
