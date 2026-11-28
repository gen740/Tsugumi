#include "history.hh"

#include <fcntl.h>
#include <flatbuffers/reflection.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <functional>
#include <set>
#include <system_error>

#include "tsugumi/protocol/runtime_state_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/log.hh"
#include "wire.hh"

namespace tsugumi {
namespace R = Tsugumi::RPC;

namespace {

std::string key(const CardPath& p) { return path_key(p); }
CardPath unkey(const std::string& s) { return path_from_key(s); }

namespace S = Tsugumi::State;

std::string waiting_bytes(const std::vector<WaitingInputData>& w) {
  S::WaitingInputsT list;
  for (const auto& wi : w) {
    auto j = std::make_unique<S::WaitingInputT>();
    j->port = wi.port;
    for (const auto& u : wi.upstream) {
      auto uj = std::make_unique<S::UpstreamT>();
      uj->card = key(u.card);
      uj->port = u.port;
      j->upstream.push_back(std::move(uj));
    }
    list.inputs.push_back(std::move(j));
  }
  return flat::encode<S::WaitingInputs>(list);
}

std::vector<WaitingInputData> waiting_from(const std::string& s) {
  std::vector<WaitingInputData> out;
  const auto list = flat::decode<S::WaitingInputs>(s);
  if (!list) return out;
  for (const auto& wi : list->inputs) {
    WaitingInputData w;
    w.port = wi->port;
    for (const auto& u : wi->upstream)
      w.upstream.push_back({unkey(u->card), u->port});
    out.push_back(std::move(w));
  }
  return out;
}

std::string detail_bytes(const InvocationSummaryData& s) {
  S::InvocationDetailT d;
  for (const auto& st : s.stages) {
    auto j = std::make_unique<S::StageT>();
    j->phase = static_cast<int>(st.phase);
    j->outcome = static_cast<int>(st.outcome);
    j->start = st.start;
    if (st.end) j->end = *st.end;
    d.stages.push_back(std::move(j));
  }
  if (s.worker_reused) d.worker_reused = *s.worker_reused;
  d.outputs_sent = s.outputs_sent;
  if (s.ready_at) d.ready_at = *s.ready_at;
  d.emulator = s.emulator;
  d.environment_id = s.environment_id;
  if (s.params) {
    d.params = std::make_unique<Tsugumi::Data::InvocationParamsT>();
    for (const auto& [name, value] : *s.params) {
      auto param = std::make_unique<Tsugumi::Data::InvocationParamT>();
      param->name = name;
      param->value = value;
      d.params->entries.push_back(std::move(param));
    }
  }
  d.execution_service = s.execution_service;
  d.execution_job_id = s.execution_job_id;
  d.execution_system = s.execution_system;
  d.execution_cpu_millis =
      s.execution_resources ? s.execution_resources->cpu_millis : 0;
  d.execution_memory_bytes =
      s.execution_resources ? s.execution_resources->memory_bytes : 0;
  d.execution_state =
      s.execution_state ? static_cast<int32_t>(*s.execution_state) : -1;
  d.batch_activation = s.batch_activation.value_or(0);
  d.batch_index = s.batch_index.value_or(0);
  d.batch_size = s.batch_size.value_or(0);
  d.implementation_index = s.implementation_index.value_or(0);
  d.implementation_label = s.implementation_label;
  d.secrets = s.secrets;
  d.build_substituter = s.build_substituter;
  d.build_pushed = s.build_pushed;
  d.execution_last_activity = static_cast<int32_t>(s.execution_last_activity);
  if (s.build_push)
    d.build_push = std::make_unique<Tsugumi::Data::BuildPushT>(*s.build_push);
  if (s.cpu_ms) d.cpu_ms = *s.cpu_ms;
  for (const auto& probe : s.worker_probes)
    d.worker_probes.push_back(
        std::make_unique<Tsugumi::Data::WorkerProbeResultT>(probe));
  for (const auto& artifact : s.artifact_outputs) {
    auto output = std::make_unique<S::InvocationArtifactOutputT>();
    output->artifact_id.assign(artifact.id.begin(), artifact.id.end());
    output->port = artifact.port;
    output->value_path = artifact.value_path;
    d.artifact_outputs.push_back(std::move(output));
  }
  return flat::encode<S::InvocationDetail>(d);
}

void detail_into(const std::string& bytes, InvocationSummaryData& s) {
  const auto decoded = flat::decode<S::InvocationDetail>(bytes);
  if (!decoded) return;
  const auto& d = *decoded;
  s.stages.clear();
  for (const auto& j : d.stages) {
    StageTimeData st;
    st.phase = static_cast<Phase>(j->phase);
    st.outcome = static_cast<StageOutcome>(j->outcome);
    st.start = j->start;
    if (j->end) st.end = *j->end;
    s.stages.push_back(st);
  }
  if (d.worker_reused) s.worker_reused = *d.worker_reused;
  s.outputs_sent = d.outputs_sent;
  s.secrets = d.secrets;
  s.build_substituter = d.build_substituter;
  s.build_pushed = d.build_pushed;
  if (d.build_push) s.build_push = *d.build_push;
  if (d.execution_last_activity > 0 &&
      d.execution_last_activity <=
          static_cast<int32_t>(ExecutionActivity::pushing))
    s.execution_last_activity =
        static_cast<ExecutionActivity>(d.execution_last_activity);
  if (d.cpu_ms) s.cpu_ms = *d.cpu_ms;
  s.worker_probes.clear();
  for (const auto& probe : d.worker_probes) s.worker_probes.push_back(*probe);
  for (const auto& artifact : d.artifact_outputs) {
    s.artifact_outputs.push_back({ArtifactKey(artifact->artifact_id.begin(),
                                              artifact->artifact_id.end()),
                                  artifact->port, artifact->value_path});
  }
  if (d.ready_at) s.ready_at = *d.ready_at;
  s.emulator = d.emulator;
  s.environment_id = d.environment_id;
  if (d.params) {
    auto& params = s.params.emplace();
    for (const auto& param : d.params->entries)
      params.emplace_back(param->name, param->value);
  }
  if (!d.execution_service.empty()) s.execution_service = d.execution_service;
  s.execution_job_id = d.execution_job_id;
  s.execution_system = d.execution_system;
  const auto cpu = d.execution_cpu_millis;
  const auto memory = d.execution_memory_bytes;
  if ((cpu || memory) && cpu <= UINT32_MAX)
    s.execution_resources =
        ExecutionResources{static_cast<uint32_t>(cpu), memory};
  const auto state = d.execution_state;
  if (state >= 0 &&
      state <= static_cast<int32_t>(RemoteExecutionState::interrupted))
    s.execution_state = static_cast<RemoteExecutionState>(state);
  if (d.batch_activation > 0) s.batch_activation = d.batch_activation;
  if (d.batch_index > 0 && d.batch_index <= UINT32_MAX)
    s.batch_index = static_cast<uint32_t>(d.batch_index);
  if (d.batch_size > 0 && d.batch_size <= UINT32_MAX)
    s.batch_size = static_cast<uint32_t>(d.batch_size);
  if (d.implementation_index > 0 && d.implementation_index <= UINT32_MAX)
    s.implementation_index = static_cast<uint32_t>(d.implementation_index);
  s.implementation_label = d.implementation_label;
}

std::string sources_bytes(const std::vector<InputSourceData>& s) {
  S::InputSourcesT list;
  for (const auto& x : s) {
    auto j = std::make_unique<S::InputSourceT>();
    j->port = x.port;
    j->card = key(x.card);
    j->invocation = x.invocation;
    list.sources.push_back(std::move(j));
  }
  return flat::encode<S::InputSources>(list);
}

std::vector<InputSourceData> sources_from(const std::string& s) {
  std::vector<InputSourceData> out;
  const auto list = flat::decode<S::InputSources>(s);
  if (!list) return out;
  for (const auto& x : list->sources)
    out.push_back({x->port, unkey(x->card), x->invocation});
  return out;
}

void bind_failure(sql::Stmt& st, int i, const std::optional<FailureData>& f) {
  if (f)
    st.bind_blob(i, f->to_bytes());
  else
    st.bind_null(i);
}

std::optional<FailureData> col_failure(const sql::Stmt& st, int c) {
  if (st.is_null(c)) return std::nullopt;
  return FailureData::from_bytes(st.blob(c));
}

std::string put_schema(sql::Db& db, const types::Type& t) {
  // Types do not change: their hash is computed once, where they are made.
  std::string h = t.hash.empty() ? crypto::sha256_hex(t.bfbs) : t.hash;
  db.prepare(
        "INSERT OR IGNORE INTO schemas(hash, root_type, bfbs) VALUES(?,?,?)")
      .bind(1, h)
      .bind(2, t.root_type)
      .bind_blob(3, t.bfbs)
      .run();
  return h;
}

RunSummaryData run_row(const sql::Stmt& st) {
  // Columns: run_id, state, rev, execution_rev, accepted_at, started_at,
  // ended_at,
  //          failure, source_hash, target, propagate, commit_id, partial_run,
  //          uses_partial_result
  RunSummaryData s;
  s.status.run_id = st.text(0);
  s.status.state = static_cast<RunState>(st.i64(1));
  s.status.rev = st.opt_u64(2);
  s.status.execution_rev = st.opt_u64(3);
  s.status.accepted_at = st.opt_i64(4);
  s.status.started_at = st.opt_i64(5);
  s.status.ended_at = st.opt_i64(6);
  s.status.failure = col_failure(st, 7);
  s.source_hash = st.text(8);
  s.target = unkey(st.text(9));
  s.propagate = st.i64(10) != 0;
  if (!st.is_null(11)) s.status.commit = st.text(11);
  s.status.partial_run = st.i64(12) != 0;
  s.status.uses_partial_result = st.i64(13) != 0;
  return s;
}

constexpr const char* kRunCols =
    "run_id, state, rev, execution_rev, accepted_at, started_at, ended_at, "
    "failure, source_hash, "
    "target, propagate, (SELECT commit_id FROM run_commits c WHERE c.run_id = "
    "runs.run_id), "
    "partial_run, uses_partial_result";

auto log_truncated_at(sql::Db& db, const std::string& run_id,
                      const CardPath& card, uint64_t invocation,
                      bool build = false) -> std::optional<uint64_t> {
  // The existing table has one row per invocation key. Build and journal
  // share a separate budget, represented by the negative invocation number.
  auto st = db.prepare(
      "SELECT limit_bytes FROM log_truncations WHERE "
      "run_id=? AND card=? AND invocation=?");
  st.bind(1, run_id)
      .bind(2, key(card))
      .bind(3, build ? -static_cast<int64_t>(invocation)
                     : static_cast<int64_t>(invocation));
  return st.step() ? st.opt_u64(0) : std::nullopt;
}

InvocationSummaryData inv_row(sql::Db& db, const sql::Stmt& st,
                              const std::string& run_id, int cached_col = -1,
                              int detail_col = -1, int service_col = -1) {
  // Columns: card, invocation, state, phase, started_at, ended_at, duration_ms,
  // failure, waiting
  InvocationSummaryData s;
  s.run_id = run_id;
  s.card = unkey(st.text(0));
  s.invocation = st.u64(1);
  s.log_truncated_at_bytes = log_truncated_at(db, run_id, s.card, s.invocation);
  if (!s.log_truncated_at_bytes)
    s.log_truncated_at_bytes = log_truncated_at(db, run_id, s.card, s.invocation, true);
  s.state = static_cast<CardState>(st.i64(2));
  if (!st.is_null(3)) s.phase = static_cast<Phase>(st.i64(3));
  s.started_at = st.opt_i64(4);
  s.ended_at = st.opt_i64(5);
  s.duration_ms = st.opt_u64(6);
  s.failure = col_failure(st, 7);
  s.waiting = waiting_from(st.blob(8));
  if (cached_col >= 0 && !st.is_null(cached_col))
    s.build_cached = st.i64(cached_col) != 0;
  if (detail_col >= 0 && !st.is_null(detail_col))
    detail_into(st.blob(detail_col), s);
  if (service_col >= 0 && !st.is_null(service_col))
    s.execution_service = st.text(service_col);
  return s;
}

bool char_start(unsigned char c) { return (c & 0xC0) != 0x80; }

// Terminal offsets reserve four bytes per Unicode scalar. A redraw can change
// a glyph's UTF-8 width without changing its position or moving later pages.
constexpr uint64_t kTerminalCellBytes = 4;
constexpr uint64_t kTerminalTileCells = 4096;

struct TerminalScope {
  std::string_view run, card;
  uint64_t invocation;
  int stream;

  auto bind(sql::Stmt& stmt) const -> sql::Stmt& {
    return stmt.bind(1, run).bind(2, card).bind(3, invocation).bind(4, stream);
  }
};

struct TerminalPosition {
  uint64_t end = 0;
  uint64_t cursor = 0;
  uint64_t line_start = 0;
  uint64_t newlines = 0;
  uint64_t redrawn_at = 0;
};

auto terminal_position(sql::Db& db, const TerminalScope& scope)
    -> TerminalPosition {
  auto select = db.prepare(
      "SELECT end_cell,cursor,line_start,newlines,redrawn_at FROM "
      "log_terminal_ends "
      "WHERE run_id=?1 AND card=?2 AND invocation=?3 AND stream=?4");
  return scope.bind(select).step()
             ? TerminalPosition{select.u64(0), select.u64(1), select.u64(2),
                                select.u64(3), select.u64(4)}
             : TerminalPosition{};
}

auto utf8_prefix(std::string_view data, uint64_t cells)
    -> std::pair<size_t, uint64_t> {
  size_t i = 0;
  uint64_t count = 0;
  for (; i < data.size(); ++i) {
    if (!char_start(static_cast<unsigned char>(data[i]))) continue;
    if (count == cells) break;
    ++count;
  }
  return {i, count};
}

auto append_terminal(sql::Db& db, const TerminalScope& scope,
                     std::string_view input, uint64_t raw_end) -> void {
  auto pos = terminal_position(db, scope);
  auto select = db.prepare(
      "SELECT data,line,starts_line FROM log_terminal_tiles WHERE "
      "run_id=?1 AND card=?2 AND invocation=?3 AND stream=?4 AND tile=?5");
  auto save = db.prepare(
      "INSERT OR REPLACE INTO log_terminal_tiles VALUES(?,?,?,?,?,?,?,?)");
  std::optional<uint64_t> tile;
  std::string data;
  uint64_t line = 0;
  bool starts_line = true;
  auto flush = [&] {
    if (!tile) return;
    scope.bind(save)
        .bind(5, *tile)
        .bind(6, line)
        .bind(7, starts_line)
        .bind_blob(8, data)
        .run();
    save.reset();
  };
  auto write = [&](std::string_view bytes) {
    while (!bytes.empty()) {
      const auto number = pos.cursor / kTerminalTileCells;
      const auto column = pos.cursor % kTerminalTileCells;
      if (tile != number) {
        flush();
        tile = number;
        if (scope.bind(select).bind(5, number).step()) {
          data = select.blob(0);
          line = select.u64(1);
          starts_line = select.i64(2) != 0;
        } else {
          data.clear();
          line = pos.newlines + 1;
          starts_line = pos.cursor == pos.line_start;
        }
        select.reset();
      }
      const auto [length, cells] =
          utf8_prefix(bytes, kTerminalTileCells - column);
      const auto part = bytes.substr(0, length);
      if (pos.cursor == pos.end) {
        data.append(part);
      } else {
        const auto begin = utf8_prefix(data, column).first;
        const auto replaced =
            utf8_prefix(std::string_view(data).substr(begin), cells).first;
        data.replace(begin, replaced, part);
        pos.redrawn_at = raw_end;
      }
      pos.cursor += cells;
      pos.end = std::max(pos.end, pos.cursor);
      const auto lf = part.rfind('\n');
      if (lf != std::string_view::npos) {
        pos.newlines += std::ranges::count(part, '\n');
        pos.line_start =
            pos.cursor - utf8_prefix(part.substr(lf + 1), cells).second;
      }
      bytes.remove_prefix(length);
    }
  };
  while (!input.empty()) {
    if (input.front() == '\r') {
      pos.cursor = pos.line_start;
      input.remove_prefix(1);
    } else if (input.front() == '\n' && pos.cursor < pos.end) {
      pos.cursor = pos.end;
      write(input.substr(0, 1));
      input.remove_prefix(1);
    } else {
      const auto mark =
          pos.cursor < pos.end ? input.find_first_of("\r\n") : input.find('\r');
      const auto count = std::min(input.size(), mark);
      write(input.substr(0, count));
      input.remove_prefix(count);
    }
  }
  flush();
  auto update = db.prepare(
      "INSERT OR REPLACE INTO log_terminal_ends VALUES(?,?,?,?,?,?,?,?,?)");
  scope.bind(update)
      .bind(5, pos.end)
      .bind(6, pos.cursor)
      .bind(7, pos.line_start)
      .bind(8, pos.newlines)
      .bind(9, pos.redrawn_at)
      .run();
}

auto read_terminal(sql::Db& db, const TerminalScope& scope, LogRead out,
                   uint64_t offset, bool tail, uint64_t limit,
                   std::optional<uint64_t> before,
                   std::optional<uint64_t> since) -> LogRead {
  out.raw_size = out.size;
  const auto pos = terminal_position(db, scope);
  out.size = pos.end * kTerminalCellBytes;
  out.reset = since && pos.redrawn_at > *since;
  tail = tail || out.reset;
  uint64_t edge = tail ? out.size : before.value_or(offset);
  if (edge > out.size || edge % kTerminalCellBytes != 0)
    throw Failed(FailureKind::invalid_range, "invalid terminal log position");
  limit -= limit % kTerminalCellBytes;
  if (tail || before) offset = edge > limit ? edge - limit : 0;
  const auto stop = tail || before ? edge : std::min(out.size, offset + limit);
  out.offset = offset;
  out.next_offset = stop;
  out.line_number = pos.newlines + 1;
  out.starts_line = pos.line_start * kTerminalCellBytes == offset;
  out.truncated = ((tail || before) && offset > 0) || stop < out.size;
  if (stop == offset) return out;

  const auto begin = offset / kTerminalCellBytes;
  const auto end = stop / kTerminalCellBytes;
  auto rows = db.prepare(
      "SELECT tile,line,starts_line,data FROM log_terminal_tiles WHERE "
      "run_id=?1 AND card=?2 AND invocation=?3 AND stream=?4 AND tile BETWEEN "
      "?5 AND ?6 "
      "ORDER BY tile");
  scope.bind(rows)
      .bind(5, begin / kTerminalTileCells)
      .bind(6, (end - 1) / kTerminalTileCells);
  auto cursor = begin;
  while (rows.step()) {
    if (rows.u64(0) != cursor / kTerminalTileCells)
      throw Failed(FailureKind::persistence_failed,
                   "discontinuous terminal log tiles");
    const std::string data = rows.blob(3);
    const auto column = cursor % kTerminalTileCells;
    const auto start = utf8_prefix(data, column).first;
    if (cursor == begin) {
      out.line_number =
          rows.u64(1) + std::count(data.begin(), data.begin() + start, '\n');
      out.starts_line =
          column == 0 ? rows.i64(2) != 0 : start > 0 && data[start - 1] == '\n';
    }
    const auto count = std::min(end - cursor, kTerminalTileCells - column);
    const auto [length, cells] =
        utf8_prefix(std::string_view(data).substr(start), count);
    out.text.append(data, start, length);
    cursor += cells;
  }
  if (cursor != end)
    throw Failed(FailureKind::persistence_failed,
                 "discontinuous terminal log tiles");
  return out;
}

// The tables a run lives in, the same in the history and in its copy
// (.tsugumi/runs.sqlite); `p` is the schema prefix ("" or "ex.").
std::string run_tables(const std::string& p) {
  return "CREATE TABLE IF NOT EXISTS " + p + R"SQL(run_counter(
  singleton INTEGER PRIMARY KEY CHECK(singleton=1), value INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(runs(
  seq INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id TEXT NOT NULL UNIQUE, op_id TEXT NOT NULL,
  state INTEGER NOT NULL, rev INTEGER, execution_rev INTEGER,
  accepted_at INTEGER, started_at INTEGER, ended_at INTEGER, failure BLOB,
  source_hash TEXT NOT NULL, target TEXT NOT NULL, propagate INTEGER NOT NULL,
  local_run INTEGER NOT NULL DEFAULT 0,
  partial_run INTEGER NOT NULL DEFAULT 0, uses_partial_result INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(build_environments(
  run_id TEXT NOT NULL, id TEXT NOT NULL, derivation TEXT NOT NULL, manifest TEXT NOT NULL,
  system TEXT NOT NULL, path_count INTEGER NOT NULL, PRIMARY KEY(run_id, id));
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(environment_paths(
  run_id TEXT NOT NULL, environment_id TEXT NOT NULL, position INTEGER NOT NULL, path TEXT NOT NULL,
  PRIMARY KEY(run_id, environment_id, position));
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(run_substituters(
  run_id TEXT NOT NULL, position INTEGER NOT NULL, url TEXT NOT NULL, PRIMARY KEY(run_id, position));
CREATE TABLE IF NOT EXISTS )SQL" +
         p +
         R"SQL(run_cards(run_id TEXT NOT NULL, card TEXT NOT NULL, PRIMARY KEY(run_id, card));
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(run_batches(
  run_id TEXT NOT NULL, card TEXT NOT NULL, activation INTEGER NOT NULL, invocation_count INTEGER NOT NULL,
  execution_list INTEGER NOT NULL, complete INTEGER NOT NULL, failed INTEGER NOT NULL, outputs BLOB NOT NULL,
  PRIMARY KEY(run_id, card, activation));
CREATE TABLE IF NOT EXISTS )SQL" +
         p +
         R"SQL(run_commits(run_id TEXT PRIMARY KEY, commit_id TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(invocations(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL,
  state INTEGER NOT NULL, phase INTEGER, started_at INTEGER, ended_at INTEGER,
  duration_ms INTEGER, failure BLOB, waiting BLOB, record_io INTEGER NOT NULL,
  input_sources BLOB, build_cached INTEGER, detail BLOB, environment_id TEXT NOT NULL,
  PRIMARY KEY(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS )SQL" +
         p +
         R"SQL(schemas(hash TEXT PRIMARY KEY, root_type TEXT NOT NULL, bfbs BLOB NOT NULL);
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(ivalues(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL, side INTEGER NOT NULL,
  port TEXT NOT NULL, schema_hash TEXT NOT NULL, root_type TEXT NOT NULL,
  size INTEGER NOT NULL, bytes BLOB, hash TEXT NOT NULL,
  PRIMARY KEY(run_id, card, invocation, side, port));
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(log_truncations(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL,
  limit_bytes INTEGER NOT NULL, PRIMARY KEY(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(logs(
  id INTEGER PRIMARY KEY AUTOINCREMENT, run_id TEXT NOT NULL, card TEXT NOT NULL,
  invocation INTEGER NOT NULL, stream INTEGER NOT NULL,
  run_off INTEGER NOT NULL, card_off INTEGER NOT NULL, inv_off INTEGER NOT NULL,
  run_lf INTEGER NOT NULL, card_lf INTEGER NOT NULL, inv_lf INTEGER NOT NULL,
  lf_count INTEGER NOT NULL, data BLOB NOT NULL);
CREATE INDEX IF NOT EXISTS )SQL" +
         p + R"SQL(logs_run ON logs(run_id, stream, run_off);
CREATE INDEX IF NOT EXISTS )SQL" +
         p + R"SQL(logs_inv ON logs(run_id, card, invocation, stream, inv_off);
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(log_terminal_ends(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL, stream INTEGER NOT NULL,
  end_cell INTEGER NOT NULL, cursor INTEGER NOT NULL, line_start INTEGER NOT NULL,
  newlines INTEGER NOT NULL, redrawn_at INTEGER NOT NULL,
  PRIMARY KEY (run_id, card, invocation, stream)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(log_terminal_tiles(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL, stream INTEGER NOT NULL,
  tile INTEGER NOT NULL, line INTEGER NOT NULL, starts_line INTEGER NOT NULL, data BLOB NOT NULL,
  PRIMARY KEY (run_id, card, invocation, stream, tile)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS )SQL" +
         p + R"SQL(artifacts(
  id BLOB NOT NULL, run_id TEXT NOT NULL, card TEXT NOT NULL,
  invocation INTEGER NOT NULL, sequence INTEGER NOT NULL, counter INTEGER NOT NULL,
  keep INTEGER NOT NULL, location TEXT NOT NULL,
  created_ms INTEGER NOT NULL, sealed_ms INTEGER, size_bytes INTEGER, tree_hash TEXT,
  PRIMARY KEY (id)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS )SQL" +
         p +
         R"SQL(artifacts_run ON artifacts(run_id, card, invocation, counter);
)SQL";
}

// Version of the local history's tables (PRAGMA user_version).
constexpr int kHistoryVersion = 23;
}  // namespace

auto History::open_live_logs() -> std::unique_ptr<sql::Db> {
  // SQLite owns and removes the temporary database when the run releases it.
  auto db = std::make_unique<sql::Db>("", false);
  db->exec(run_tables(""));
  return db;
}

History::History(const std::filesystem::path& db_path, std::string fault,
                 const std::filesystem::path& run_scratch)
    : path_(db_path), fault_(std::move(fault)) {
  writer_db_ = std::make_unique<sql::Db>(path_.string(),
                                         sql::FormatVersion{kHistoryVersion});
  // Single process owns the database (guarded by the project lock).
  writer_db_->exec("PRAGMA journal_mode=WAL");
  writer_db_->exec("PRAGMA synchronous=FULL");
  writer_db_->exec("PRAGMA foreign_keys=ON");
  writer_db_->initialize_format(kHistoryVersion, [&] {
    init_schema(*writer_db_);
    // Files may outlive a moved-aside history. Reserve their run numbers so a
    // new run cannot overwrite them or remove them through its cleanup action.
    uint64_t last_run = 0;
    const auto scratch_runs = run_scratch.empty() ? std::filesystem::path{}
                                                  : run_scratch / "runs";
    for (const auto& directory :
         {path_.parent_path() / "artifacts", scratch_runs}) {
      if (directory.empty() || !std::filesystem::is_directory(
                                   std::filesystem::symlink_status(directory)))
        continue;
      for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (!crypto::is_run_id(name)) continue;
        uint64_t number = 0;
        const auto result =
            std::from_chars(name.data(), name.data() + name.size(), number);
        if (result.ec == std::errc{}) last_run = std::max(last_run, number);
      }
    }
    if (last_run)
      writer_db_->prepare("INSERT INTO run_counter VALUES(1,?)")
          .bind(1, last_run)
          .run();
  });
  writer_ = std::thread([this] { writer_loop(); });
}

History::~History() {
  {
    std::lock_guard<std::mutex> l(wmu_);
    stopping_ = true;
  }
  wcv_.notify_all();
  if (writer_.joinable()) writer_.join();
}

void History::init_schema(sql::Db& db) {
  db.exec(run_tables("") + R"SQL(
CREATE INDEX IF NOT EXISTS logs_card ON logs(run_id, card, stream, card_off);
CREATE TABLE IF NOT EXISTS run_definition_paths(
  run_id TEXT NOT NULL, card TEXT NOT NULL, path TEXT NOT NULL,
  PRIMARY KEY(run_id, card));
CREATE TABLE IF NOT EXISTS invocation_sequences(
  sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL,
  UNIQUE(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS active_environments(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL, environment_id TEXT NOT NULL,
  PRIMARY KEY(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS statuses(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL,
  state INTEGER NOT NULL, phase INTEGER, started_at INTEGER, failure BLOB, waiting BLOB,
  updated_at INTEGER NOT NULL, execution_service TEXT NOT NULL, detail BLOB NOT NULL,
  PRIMARY KEY(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS remote_jobs(
  job_id TEXT PRIMARY KEY, run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL,
  instance TEXT NOT NULL, fingerprint TEXT NOT NULL,
  UNIQUE(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS remote_releases(
  job_id TEXT PRIMARY KEY, service TEXT NOT NULL,
  instance TEXT NOT NULL, fingerprint TEXT NOT NULL, delete_data INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS last_inputs(
  card TEXT PRIMARY KEY, run_id TEXT NOT NULL, invocation INTEGER NOT NULL, inputs BLOB NOT NULL);
CREATE TABLE IF NOT EXISTS receipts(
  op_id TEXT PRIMARY KEY, kind TEXT NOT NULL, digest TEXT NOT NULL, result BLOB NOT NULL);
)SQL");
}

History::ReaderLease History::acquire_reader() {
  std::unique_ptr<sql::Db> db;
  {
    std::lock_guard<std::mutex> l(rmu_);
    if (!reader_pool_.empty()) {
      db = std::move(reader_pool_.back());
      reader_pool_.pop_back();
    }
  }
  if (!db) {
    db = std::make_unique<sql::Db>(path_.string(), true);
    db->exec("PRAGMA query_only=ON");
  }
  return ReaderLease{this, std::move(db)};
}

void History::release_reader(std::unique_ptr<sql::Db> db) {
  std::lock_guard<std::mutex> l(rmu_);
  constexpr size_t kMaxIdleReaders = 16;
  if (reader_pool_.size() < kMaxIdleReaders) {
    reader_pool_.push_back(std::move(db));
  }
}

void History::submit(Work work, std::function<void(bool)> done) {
  std::unique_lock<std::mutex> l(wmu_);
  wspace_.wait(l, [&] { return wq_.size() < 1024 || stopping_; });
  wq_.emplace_back(std::move(work), std::move(done));
  wcv_.notify_one();
}

void History::flush() {
  try {
    write_sync([](sql::Db&) {});
  } catch (const Failed&) {
  }
}

auto History::allocate_invocation_sequence(
    const InvocationSummaryData& invocation) -> uint64_t {
  uint64_t sequence = 0;
  write_sync([&](sql::Db& db) {
    auto insert = db.prepare(
        "INSERT INTO invocation_sequences(run_id,card,invocation) "
        "VALUES(?,?,?) RETURNING sequence");
    insert.bind(1, invocation.run_id);
    insert.bind(2, key(invocation.card));
    insert.bind(3, static_cast<int64_t>(invocation.invocation));
    if (!insert.step()) throw std::runtime_error("missing invocation sequence");
    sequence = static_cast<uint64_t>(insert.i64(0));
  });
  return sequence;
}

void History::write_sync(const Work& work) {
  std::mutex m;
  std::condition_variable cv;
  bool finished = false;
  bool ok = false;
  submit(work, [&](bool r) {
    std::lock_guard<std::mutex> l(m);
    ok = r;
    finished = true;
    cv.notify_one();
  });
  std::unique_lock<std::mutex> l(m);
  cv.wait(l, [&] { return finished; });
  if (!ok)
    throw Failed(FailureKind::persistence_failed, "history write failed");
}

void History::writer_loop() {
  while (true) {
    std::vector<std::pair<Work, std::function<void(bool)>>> batch;
    {
      std::unique_lock<std::mutex> l(wmu_);
      wcv_.wait(l, [&] { return stopping_ || !wq_.empty(); });
      if (wq_.empty() && stopping_) return;
      // Group a few queued writes into one transaction; each keeps its order.
      while (!wq_.empty() && batch.size() < 64) {
        batch.push_back(std::move(wq_.front()));
        wq_.pop_front();
      }
      wspace_.notify_all();
    }
    bool ok = true;
    try {
      sql::Transaction t(*writer_db_);
      for (auto& [work, _] : batch) work(*writer_db_);
      t.commit();
    } catch (const std::exception& e) {
      log::error(std::string("history batch failed, retrying individually: ") +
                 e.what());
      ok = false;
    }
    if (!ok) {
      // Retry one by one so a single bad write does not lose the others.
      for (auto& [work, done] : batch) {
        bool one = true;
        try {
          sql::Transaction t(*writer_db_);
          work(*writer_db_);
          t.commit();
        } catch (const std::exception& e) {
          log::error(std::string("history write failed: ") + e.what());
          one = false;
        }
        if (done) done(one);
      }
      continue;
    }
    for (auto& [_, done] : batch)
      if (done) done(true);
  }
}

auto History::put_environment(sql::Db& db, const std::string& run_id,
                              const BuildEnvironmentData& e) -> std::string {
  auto existing = db.prepare(
      "SELECT id FROM build_environments WHERE run_id=? "
      "AND derivation=? AND manifest=? AND system=?");
  existing.bind(1, run_id)
      .bind(2, e.derivation)
      .bind(3, e.manifest)
      .bind(4, e.system);
  if (existing.step()) return existing.text(0);
  auto count =
      db.prepare("SELECT count(*) FROM build_environments WHERE run_id=?");
  count.bind(1, run_id);
  count.step();
  const auto id = std::to_string(count.i64(0) + 1);
  db.prepare("INSERT INTO build_environments VALUES(?,?,?,?,?,?)")
      .bind(1, run_id)
      .bind(2, id)
      .bind(3, e.derivation)
      .bind(4, e.manifest)
      .bind(5, e.system)
      .bind(6, static_cast<uint64_t>(e.paths.size()))
      .run();
  auto insert = db.prepare("INSERT INTO environment_paths VALUES(?,?,?,?)");
  for (size_t i = 0; i < e.paths.size(); ++i) {
    insert.bind(1, run_id)
        .bind(2, id)
        .bind(3, static_cast<uint64_t>(i))
        .bind(4, e.paths[i])
        .run();
    insert.reset();
  }
  return id;
}

auto History::put_substituters(sql::Db& db, const std::string& run_id,
                               const std::vector<std::string>& urls) -> void {
  auto insert =
      db.prepare("INSERT OR IGNORE INTO run_substituters VALUES(?,?,?)");
  for (size_t i = 0; i < urls.size(); ++i) {
    insert.bind(1, run_id)
        .bind(2, static_cast<uint64_t>(i))
        .bind(3, urls[i])
        .run();
    insert.reset();
  }
}

auto History::next_run_id(sql::Db& db) -> std::string {
  db.exec("INSERT OR IGNORE INTO run_counter VALUES(1,0)");
  auto q = db.prepare(
      "UPDATE run_counter SET value=value+1 WHERE singleton=1 "
      "AND value<9223372036854775807 RETURNING value");
  if (!q.step()) throw std::runtime_error("project run numbers exhausted");
  return std::to_string(q.i64(0));
}

void History::insert_run(sql::Db& db, const RunSummaryData& s,
                         const std::string& op_id) {
  db.prepare(
        "INSERT INTO runs(run_id, op_id, state, rev, execution_rev, "
        "accepted_at, started_at, "
        "ended_at, failure, source_hash, target, propagate, local_run, "
        "partial_run, uses_partial_result) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,1,?,?)")
      .bind(1, *s.status.run_id)
      .bind(2, op_id)
      .bind(3, static_cast<int>(s.status.state))
      .bind(4, s.status.rev)
      .bind(5, s.status.execution_rev)
      .bind(6, s.status.accepted_at)
      .bind(7, s.status.started_at)
      .bind(8, s.status.ended_at)
      .bind_null(9)
      .bind(10, s.source_hash)
      .bind(11, key(s.target))
      .bind(12, s.propagate)
      .bind(13, s.status.partial_run)
      .bind(14, s.status.uses_partial_result)
      .run();
  if (s.status.commit)
    db.prepare(
          "INSERT OR REPLACE INTO run_commits(run_id, commit_id) VALUES(?,?)")
        .bind(1, *s.status.run_id)
        .bind(2, *s.status.commit)
        .run();
  if (s.status.failure) update_run(db, s.status);
}

void History::update_run(sql::Db& db, const RunStatusData& s) {
  auto st = db.prepare(
      "UPDATE runs SET state=?, started_at=?, ended_at=?, failure=?, "
      "partial_run=?, uses_partial_result=? WHERE run_id=?");
  st.bind(1, static_cast<int>(s.state))
      .bind(2, s.started_at)
      .bind(3, s.ended_at);
  bind_failure(st, 4, s.failure);
  st.bind(5, s.partial_run)
      .bind(6, s.uses_partial_result)
      .bind(7, *s.run_id)
      .run();
}

void History::put_status(sql::Db& db, const InvocationSummaryData& s) {
  if (!s.environment_id.empty())
    db.prepare("INSERT OR REPLACE INTO active_environments VALUES(?,?,?,?)")
        .bind(1, s.run_id)
        .bind(2, key(s.card))
        .bind(3, s.invocation)
        .bind(4, s.environment_id)
        .run();
  auto st = db.prepare(
      "INSERT INTO statuses(run_id, card, invocation, state, phase, "
      "started_at, failure, waiting, updated_at, execution_service, detail) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(run_id, card, invocation) DO "
      "UPDATE SET "
      "state=excluded.state, phase=excluded.phase, "
      "started_at=excluded.started_at, "
      "failure=excluded.failure, waiting=excluded.waiting, "
      "updated_at=excluded.updated_at, "
      "execution_service=excluded.execution_service, detail=excluded.detail");
  st.bind(1, s.run_id)
      .bind(2, key(s.card))
      .bind(3, s.invocation)
      .bind(4, static_cast<int>(s.state));
  if (s.phase)
    st.bind(5, static_cast<int>(*s.phase));
  else
    st.bind_null(5);
  st.bind(6, s.started_at);
  bind_failure(st, 7, s.failure);
  st.bind_blob(8, waiting_bytes(s.waiting))
      .bind(9, now_ms())
      .bind(10, s.execution_service)
      .bind_blob(11, detail_bytes(s))
      .run();
  db.prepare("INSERT OR IGNORE INTO run_cards(run_id, card) VALUES(?,?)")
      .bind(1, s.run_id)
      .bind(2, key(s.card))
      .run();
}

auto History::put_batch(sql::Db& db, const RunBatchData& batch) -> void {
  S::BatchOutputsT outputs;
  for (const auto& output : batch.outputs) {
    auto entry = std::make_unique<S::BatchOutputT>();
    entry->port = output.port;
    entry->count = output.count;
    entry->execution_list = output.execution_list;
    outputs.outputs.push_back(std::move(entry));
  }
  const auto card = key(batch.card);
  db.prepare("INSERT OR REPLACE INTO run_batches VALUES(?,?,?,?,?,?,?,?)")
      .bind(1, batch.run_id)
      .bind(2, card)
      .bind(3, batch.activation)
      .bind(4, static_cast<uint64_t>(batch.invocation_count))
      .bind(5, batch.execution_list)
      .bind(6, batch.complete)
      .bind(7, batch.failed)
      .bind_blob(8, flat::encode<S::BatchOutputs>(outputs))
      .run();
  db.prepare("INSERT OR IGNORE INTO run_cards(run_id, card) VALUES(?,?)")
      .bind(1, batch.run_id)
      .bind(2, card)
      .run();
}

auto History::interrupt_batches(sql::Db& db, const std::string& run_id)
    -> void {
  db.prepare(
        "UPDATE run_batches SET complete=1, failed=1, outputs=? WHERE "
        "run_id=? AND complete=0")
      .bind_blob(1, flat::encode<S::BatchOutputs>({}))
      .bind(2, run_id)
      .run();
}

auto History::put_remote_job(sql::Db& db, const RemoteJobRecord& job) -> void {
  const auto& inv = job.invocation;
  db.prepare("INSERT INTO remote_jobs VALUES(?,?,?,?,?,?)")
      .bind(1, inv.execution_job_id)
      .bind(2, inv.run_id)
      .bind(3, key(inv.card))
      .bind(4, inv.invocation)
      .bind(5, job.instance)
      .bind(6, crypto::sha256_hex(job.spec))
      .run();
}

auto History::has_remote_jobs(sql::Db& db) -> bool {
  auto st = db.prepare("SELECT 1 FROM remote_jobs LIMIT 1");
  return st.step();
}

void History::complete_invocation(sql::Db& db,
                                  const InvocationRecordData& r) const {
  if (fault_ == "history_invocation")
    throw sql::Error("injected fault: invocation record write failed");
  const auto& s = r.summary;
  auto st = db.prepare(
      "INSERT OR REPLACE INTO invocations(run_id, card, "
      "invocation, state, phase, started_at, ended_at, "
      "duration_ms, failure, waiting, record_io, "
      "input_sources, build_cached, detail, environment_id) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  st.bind(1, s.run_id)
      .bind(2, key(s.card))
      .bind(3, s.invocation)
      .bind(4, static_cast<int>(s.state));
  if (s.phase)
    st.bind(5, static_cast<int>(*s.phase));
  else
    st.bind_null(5);
  st.bind(6, s.started_at).bind(7, s.ended_at).bind(8, s.duration_ms);
  bind_failure(st, 9, s.failure);
  st.bind_blob(10, waiting_bytes(s.waiting))
      .bind(11, r.record_io)
      .bind_blob(12, sources_bytes(r.input_sources));
  if (s.build_cached)
    st.bind(13, *s.build_cached);
  else
    st.bind_null(13);
  st.bind_blob(14, detail_bytes(s)).bind(15, s.environment_id).run();
  // The completion removes exactly this invocation's progress status, never a
  // newer one.
  db.prepare(
        "DELETE FROM active_environments WHERE run_id=? AND card=? AND "
        "invocation=?")
      .bind(1, s.run_id)
      .bind(2, key(s.card))
      .bind(3, s.invocation)
      .run();
  db.prepare("DELETE FROM statuses WHERE run_id=? AND card=? AND invocation=?")
      .bind(1, s.run_id)
      .bind(2, key(s.card))
      .bind(3, s.invocation)
      .run();
  if (!s.execution_job_id.empty()) {
    auto pending = db.prepare(
        "SELECT instance,fingerprint FROM remote_jobs WHERE "
        "job_id=?");
    pending.bind(1, s.execution_job_id);
    if (pending.step())
      db.prepare("INSERT OR REPLACE INTO remote_releases VALUES(?,?,?,?,?)")
          .bind(1, s.execution_job_id)
          .bind(2, s.execution_service)
          .bind(3, pending.text(0))
          .bind(4, pending.text(1))
          .bind(5, !s.failure || s.failure->kind != FailureKind::interrupted)
          .run();
    db.prepare("DELETE FROM remote_jobs WHERE job_id=?")
        .bind(1, s.execution_job_id)
        .run();
  }
  db.prepare("INSERT OR IGNORE INTO run_cards(run_id, card) VALUES(?,?)")
      .bind(1, s.run_id)
      .bind(2, key(s.card))
      .run();
  {
    auto put = [&](const Value& v, int side) {
      const auto schema = v.type ? put_schema(db, *v.type) : "";
      db.prepare(
            "INSERT OR REPLACE INTO ivalues(run_id, card, invocation, "
            "side, port, schema_hash, root_type, size, bytes, hash) "
            "VALUES(?,?,?,?,?,?,?,?,?,?)")
          .bind(1, s.run_id)
          .bind(2, key(s.card))
          .bind(3, s.invocation)
          .bind(4, side)
          .bind(5, v.name)
          .bind(6, schema)
          .bind(7, v.type ? v.type->root_type : std::string())
          .bind(8, static_cast<uint64_t>(v.bytes.size()))
          .bind_blob(9, v.bytes)
          .bind(10, crypto::sha256_hex(v.bytes))
          .run();
    };
    for (const auto& v : r.inputs) put(v, 0);
    for (const auto& v : r.outputs) put(v, 1);
  }
}

auto History::put_last_input(sql::Db& db, const CardPath& card,
                             const std::string& run_id, uint64_t activation,
                             const std::vector<SavedInputGroup>& groups)
    -> void {
  S::SavedInputsT list;
  for (const auto& group : groups) {
    auto entry = std::make_unique<S::SavedInputGroupT>();
    entry->port = group.port;
    entry->execution_list = group.execution_list;
    entry->partial_result = group.partial_result;
    for (const auto& v : group.values) {
      auto j = std::make_unique<S::SavedInputT>();
      j->schema = v.type ? put_schema(db, *v.type) : "";
      j->root_type = v.type ? v.type->root_type : "";
      j->data.assign(v.bytes.begin(), v.bytes.end());
      j->schema_id = v.schema;
      entry->values.push_back(std::move(j));
    }
    list.groups.push_back(std::move(entry));
  }
  db.prepare(
        "INSERT OR REPLACE INTO last_inputs(card, run_id, invocation, "
        "inputs) VALUES(?,?,?,?)")
      .bind(1, key(card))
      .bind(2, run_id)
      .bind(3, activation)
      .bind_blob(4, flat::encode<S::SavedInputs>(list))
      .run();
}

void History::put_artifact(sql::Db& db, const ArtifactData& a) {
  auto insert = db.prepare(
      "INSERT INTO artifacts(id, run_id, card, invocation, sequence, counter, "
      "keep, location, "
      "created_ms, sealed_ms, size_bytes, tree_hash) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.bind_blob(1, a.id)
      .bind(2, a.run_id)
      .bind(3, key(a.card))
      .bind(4, a.invocation)
      .bind(5, a.sequence)
      .bind(6, a.counter)
      .bind(7, a.keep)
      .bind(8, a.location)
      .bind(9, a.created_ms)
      .bind(10, a.sealed_ms)
      .bind(11, a.size_bytes)
      .bind(12, a.tree_hash)
      .run();
}

void History::put_receipt(sql::Db& db, const std::string& op_id,
                          const std::string& kind, const std::string& digest,
                          const std::string& bytes) {
  db.prepare(
        "INSERT OR REPLACE INTO receipts(op_id, kind, digest, result) "
        "VALUES(?,?,?,?)")
      .bind(1, op_id)
      .bind(2, kind)
      .bind(3, digest)
      .bind_blob(4, bytes)
      .run();

  // Bound replay payloads the same way as document receipts. Shell
  // uncertainty markers must remain durable because replaying the operation
  // could repeat an external side effect.
  constexpr int kReceiptCount = 2000;
  constexpr int kReceiptBytes = 2 << 20;
  std::vector<int64_t> expired;
  {
    auto rows = db.prepare(R"sql(
      SELECT rowid, kind, CASE WHEN kind = 'shell' THEN result END FROM receipts
      WHERE rowid IN (
        SELECT rowid FROM (
          SELECT rowid, row_number() OVER recent AS count,
            sum(length(result) + length(CAST(op_id AS BLOB)) +
                length(kind) + length(digest)) OVER recent AS size
          FROM receipts WHERE length(result) > 0
          WINDOW recent AS (ORDER BY rowid DESC)
        ) WHERE count > 1 AND (count > ? OR size > ?)
      )
    )sql");
    rows.bind(1, kReceiptCount).bind(2, kReceiptBytes);
    while (rows.step()) {
      if (rows.text(1) == "shell") {
        const auto result = rows.blob(2);
        flatbuffers::Verifier verifier(
            reinterpret_cast<const uint8_t*>(result.data()), result.size());
        if (!verifier.VerifyBuffer<R::OperationReceipt>(nullptr)) continue;
        const auto* receipt =
            flatbuffers::GetRoot<R::OperationReceipt>(result.data());
        const auto* failure = receipt->result_as_Failure();
        if (failure && failure->kind() == FailureKind::interrupted) continue;
      }
      expired.push_back(rows.i64(0));
    }
  }
  // Keep the idempotency key even after discarding the reply payload.
  auto remove = db.prepare("UPDATE receipts SET result=X'' WHERE rowid=?");
  for (const auto row : expired) {
    remove.reset();
    remove.bind(1, row).run();
  }
}

std::vector<LogEnd> History::append_log(sql::Db& db, const std::string& run_id,
                                        const CardPath& card,
                                        uint64_t invocation, LogStream stream,
                                        const std::string& text) {
  if (text.empty()) return {};
  int s = static_cast<int>(stream);
  std::string ck = key(card);
  auto load = [&](const char* sql_text, bool with_card, bool with_inv) {
    auto st = db.prepare(sql_text);
    int i = 1;
    st.bind(i++, run_id);
    if (with_card) st.bind(i++, ck);
    if (with_inv) st.bind(i++, invocation);
    st.bind(i++, s);
    return st.step() ? LogPosition{st.u64(0), st.u64(1)} : LogPosition{};
  };
  auto r0 = load(
      "SELECT run_off + length(data), run_lf + lf_count FROM logs "
      "WHERE run_id=? AND stream=? ORDER BY run_off DESC LIMIT 1",
      false, false);
  LogPosition c0, i0;
  if (!card.empty()) {
    c0 = load(
        "SELECT card_off + length(data), card_lf + lf_count FROM logs "
        "WHERE run_id=? AND card=? AND stream=? "
        "ORDER BY card_off DESC LIMIT 1",
        true, false);
    i0 = load(
        "SELECT inv_off + length(data), inv_lf + lf_count FROM logs "
        "WHERE run_id=? AND card=? AND invocation=? AND stream=? "
        "ORDER BY inv_off DESC LIMIT 1",
        true, true);
  }
  auto insert = db.prepare(
      "INSERT INTO logs(run_id, card, invocation, stream, run_off, "
      "card_off, inv_off, run_lf, card_lf, inv_lf, lf_count, data) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
  // Bound the extra bytes read and counted around a page boundary, even
  // when one remote log entry is larger than the requested page.
  constexpr size_t chunk_bytes = 64 * 1024;
  for (std::string_view rest(text); !rest.empty();) {
    const auto chunk = rest.substr(0, chunk_bytes);
    const auto lf_count =
        static_cast<uint64_t>(std::ranges::count(chunk, '\n'));
    insert.bind(1, run_id)
        .bind(2, ck)
        .bind(3, invocation)
        .bind(4, s)
        .bind(5, r0.offset)
        .bind(6, c0.offset)
        .bind(7, i0.offset)
        .bind(8, r0.newlines)
        .bind(9, c0.newlines)
        .bind(10, i0.newlines)
        .bind(11, lf_count)
        .bind_blob(12, chunk)
        .run();
    insert.reset();
    for (auto* position : {&r0, &c0, &i0}) {
      position->offset += chunk.size();
      position->newlines += lf_count;
    }
    rest.remove_prefix(chunk.size());
  }
  append_terminal(db, {run_id, "", 0, s}, text, r0.offset);
  if (!card.empty()) {
    append_terminal(db, {run_id, ck, 0, s}, text, c0.offset);
    append_terminal(db, {run_id, ck, invocation, s}, text, i0.offset);
  }
  std::vector<LogEnd> ends{
      {run_id, std::nullopt, std::nullopt, stream, r0.offset}};
  if (!card.empty())
    ends.push_back({run_id, card, invocation, stream, i0.offset});
  return ends;
}

auto History::mark_log_truncated(sql::Db& db, const std::string& run_id,
                                 const CardPath& card, uint64_t invocation,
                                 uint64_t limit_bytes, bool build) -> void {
  db.prepare(
        "INSERT OR IGNORE INTO log_truncations(run_id, card, invocation, "
        "limit_bytes) VALUES(?,?,?,?)")
      .bind(1, run_id)
      .bind(2, key(card))
      .bind(3, build ? -static_cast<int64_t>(invocation)
                     : static_cast<int64_t>(invocation))
      .bind(4, limit_bytes)
      .run();
}

std::optional<RunSummaryData> History::run(sql::Db& db,
                                           const std::string& run_id) {
  auto st = db.prepare(std::string("SELECT ") + kRunCols +
                       " FROM runs WHERE run_id=?");
  st.bind(1, run_id);
  if (!st.step()) return std::nullopt;
  return run_row(st);
}

std::optional<RunSummaryData> History::latest_run(sql::Db& db) {
  // Import sequence and clocks on other machines do not describe local scratch.
  auto st =
      db.prepare(std::string("SELECT ") + kRunCols +
                 " FROM runs WHERE local_run=1 ORDER BY seq DESC LIMIT 1");
  if (!st.step()) return std::nullopt;
  return run_row(st);
}

std::optional<History::ReceiptRow> History::receipt(sql::Db& db,
                                                    const std::string& op_id) {
  auto st =
      db.prepare("SELECT kind, digest, result FROM receipts WHERE op_id=?");
  st.bind(1, op_id);
  if (!st.step()) return std::nullopt;
  auto bytes = st.blob(2);
  if (bytes.empty()) {
    flatbuffers::FlatBufferBuilder builder;
    const auto failure = wire::failure(
        builder,
        FailureData::make(FailureKind::operation_conflict,
                          "the operation was applied and its reply expired"));
    builder.Finish(
        R::CreateOperationReceipt(builder, builder.CreateString(op_id),
                                  R::ReceiptValue::Failure, failure.Union()));
    bytes.assign(reinterpret_cast<const char*>(builder.GetBufferPointer()),
                 builder.GetSize());
  }
  return ReceiptRow{st.text(0), st.text(1), std::move(bytes)};
}

std::vector<InvocationSummaryData> History::latest_invocations(
    sql::Db& db, const std::string& run_id) {
  auto st = db.prepare(
      "SELECT card, invocation, state, phase, started_at, ended_at, "
      "duration_ms, failure, waiting, build_cached, detail, NULL "
      "FROM invocations i WHERE run_id=?1 AND invocation=(SELECT "
      "MAX(invocation) FROM invocations WHERE run_id=?1 AND card=i.card) "
      "UNION ALL SELECT card, invocation, state, phase, started_at, NULL, "
      "NULL, failure, waiting, NULL, detail, execution_service "
      "FROM statuses s WHERE run_id=?1 AND invocation=(SELECT MAX(invocation) "
      "FROM statuses WHERE run_id=?1 AND card=s.card) "
      "AND invocation > COALESCE((SELECT MAX(invocation) FROM invocations "
      "WHERE run_id=?1 AND card=s.card), 0) "
      "ORDER BY card, invocation");
  st.bind(1, run_id);
  std::vector<InvocationSummaryData> out;
  while (st.step()) {
    auto row = inv_row(db, st, run_id, 9, 10, 11);
    // A status newer than the Card's last record is the one a client wants.
    if (!out.empty() && out.back().card == row.card)
      out.back() = std::move(row);
    else
      out.push_back(std::move(row));
  }
  return out;
}

namespace {

auto batch_row(const sql::Stmt& row, const std::string& run_id)
    -> RunBatchData {
  RunBatchData batch;
  batch.run_id = run_id;
  batch.card = unkey(row.text(0));
  batch.activation = row.u64(1);
  const auto count = row.u64(2);
  if (count > UINT32_MAX)
    throw std::runtime_error("invalid invocation count in run batch history");
  batch.invocation_count = static_cast<uint32_t>(count);
  batch.execution_list = row.i64(3) != 0;
  batch.complete = row.i64(4) != 0;
  batch.failed = row.i64(5) != 0;
  const auto outputs = flat::decode<S::BatchOutputs>(row.blob(6));
  if (!outputs)
    throw std::runtime_error("invalid outputs in run batch history");
  for (const auto& entry : outputs->outputs) {
    if (entry->count > UINT32_MAX)
      throw std::runtime_error("invalid output count in run batch history");
    batch.outputs.push_back({entry->port, static_cast<uint32_t>(entry->count),
                             entry->execution_list});
  }
  return batch;
}

}  // namespace

auto History::latest_batches(sql::Db& db, const std::string& run_id,
                             const std::optional<CardPath>& card)
    -> std::vector<RunBatchData> {
  auto st = db.prepare(
      "SELECT card, activation, invocation_count, execution_list, "
      "complete, failed, outputs FROM run_batches b "
      "WHERE run_id=?1 AND (?2 IS NULL OR card=?2) AND "
      "activation=(SELECT MAX(activation) FROM run_batches "
      "WHERE run_id=?1 AND card=b.card) ORDER BY card");
  st.bind(1, run_id);
  if (card)
    st.bind(2, key(*card));
  else
    st.bind_null(2);
  std::vector<RunBatchData> out;
  while (st.step()) out.push_back(batch_row(st, run_id));
  return out;
}

std::vector<InvocationSummaryData> History::invocations(
    sql::Db& db, const std::string& run_id, const CardPath& card,
    uint64_t after, uint64_t limit) {
  // Completed records and in-progress statuses, ascending invocation number.
  auto st = db.prepare(
      "SELECT card, invocation, state, phase, started_at, ended_at, "
      "duration_ms, failure, waiting, build_cached, detail, NULL "
      "FROM invocations WHERE run_id=?1 AND card=?2 AND invocation>?3 "
      "UNION ALL SELECT card, invocation, state, phase, started_at, NULL, "
      "NULL, failure, waiting, NULL, detail, execution_service "
      "FROM statuses WHERE run_id=?1 AND card=?2 AND invocation>?3 AND "
      "invocation NOT IN "
      "(SELECT invocation FROM invocations WHERE run_id=?1 AND card=?2) "
      "ORDER BY invocation LIMIT ?4");
  st.bind(1, run_id).bind(2, key(card)).bind(3, after).bind(4, limit);
  std::vector<InvocationSummaryData> out;
  while (st.step()) out.push_back(inv_row(db, st, run_id, 9, 10, 11));
  return out;
}

std::optional<InvocationRecordData> History::invocation(
    sql::Db& db, const std::string& run_id, const CardPath& card, uint64_t n) {
  auto st = db.prepare(
      "SELECT card, invocation, state, phase, started_at, ended_at, "
      "duration_ms, failure, waiting, "
      "record_io, input_sources, build_cached, detail FROM "
      "invocations WHERE run_id=? AND card=? AND invocation=?");
  st.bind(1, run_id).bind(2, key(card)).bind(3, n);
  if (!st.step()) return std::nullopt;
  InvocationRecordData r;
  r.summary = inv_row(db, st, run_id, 11, 12);
  r.record_io = st.i64(9) != 0;
  r.input_sources = sources_from(st.blob(10));
  return r;
}

std::optional<InvocationSummaryData> History::status(sql::Db& db,
                                                     const std::string& run_id,
                                                     const CardPath& card,
                                                     uint64_t n) {
  auto st = db.prepare(
      "SELECT card, invocation, state, phase, started_at, NULL, "
      "NULL, failure, waiting, detail, execution_service "
      "FROM statuses WHERE run_id=? AND card=? AND invocation=?");
  st.bind(1, run_id).bind(2, key(card)).bind(3, n);
  if (!st.step()) return std::nullopt;
  return inv_row(db, st, run_id, -1, 9, 10);
}

std::optional<uint64_t> History::latest_invocation(sql::Db& db,
                                                   const std::string& run_id,
                                                   const CardPath& card,
                                                   bool include_status) {
  std::string q =
      "SELECT MAX(invocation) FROM (SELECT invocation FROM "
      "invocations WHERE run_id=?1 AND card=?2";
  if (include_status)
    q += " UNION ALL SELECT invocation FROM statuses WHERE run_id=?1 AND "
         "card=?2";
  q += ")";
  auto st = db.prepare(q);
  st.bind(1, run_id).bind(2, key(card));
  if (!st.step() || st.is_null(0)) return std::nullopt;
  return st.u64(0);
}

std::optional<uint64_t> History::latest_completed(sql::Db& db,
                                                  const std::string& run_id,
                                                  const CardPath& card) {
  return latest_invocation(db, run_id, card, false);
}

bool History::run_has_card(sql::Db& db, const std::string& run_id,
                           const CardPath& card) {
  auto st = db.prepare(
      "SELECT 1 FROM run_cards WHERE run_id=? AND card=? UNION ALL "
      "SELECT 1 FROM runs WHERE run_id=? AND target=? LIMIT 1");
  st.bind(1, run_id).bind(2, key(card)).bind(3, run_id).bind(4, key(card));
  return st.step();
}

History::Aggregate History::aggregate(sql::Db& db, const std::string& run_id,
                                      const CardPath& card) {
  auto st = db.prepare(
      "SELECT COUNT(*), SUM(state=?3), SUM(state=?4), "
      "SUM(state=?5), COALESCE(SUM(duration_ms),0) "
      "FROM invocations WHERE run_id=?1 AND card=?2");
  st.bind(1, run_id)
      .bind(2, key(card))
      .bind(3, static_cast<int>(CardState::succeeded))
      .bind(4, static_cast<int>(CardState::failed))
      .bind(5, static_cast<int>(CardState::cancelled));
  Aggregate a;
  if (st.step()) {
    a.calls = st.u64(0);
    a.succeeded = st.is_null(1) ? 0 : st.u64(1);
    a.failed = st.is_null(2) ? 0 : st.u64(2);
    a.cancelled = st.is_null(3) ? 0 : st.u64(3);
    a.total_ms = st.u64(4);
  }
  return a;
}

std::optional<FailureData> History::latest_failure(sql::Db& db,
                                                   const std::string& run_id,
                                                   const CardPath& card) {
  auto st = db.prepare(
      "SELECT failure FROM invocations WHERE run_id=? AND card=? AND state=? "
      "ORDER BY invocation DESC LIMIT 1");
  st.bind(1, run_id)
      .bind(2, key(card))
      .bind(3, static_cast<int>(CardState::failed));
  if (!st.step()) return std::nullopt;
  return col_failure(st, 0);
}

namespace {

using ArtifactValuePaths = std::set<std::pair<ArtifactKey, std::string>>;

auto value_artifact_paths(const reflection::Schema& schema,
                          const reflection::Object& root, const uint8_t* data)
    -> ArtifactValuePaths {
  if (!schema.objects()->LookupByKey("tsugumi.ArtifactId")) return {};
  ArtifactValuePaths paths;
  std::set<std::pair<const uint8_t*, const reflection::Object*>> visited;
  const auto visit = [&](const auto& self, const reflection::Object& object,
                         const uint8_t* address,
                         const std::string& path) -> void {
    if (!address || !visited.emplace(address, &object).second) return;
    const auto* table = reinterpret_cast<const flatbuffers::Table*>(address);
    if (object.is_struct() && object.bytesize() == 16 &&
        object.name()->string_view() == "tsugumi.ArtifactId") {
      paths.emplace(ArtifactKey(reinterpret_cast<const char*>(address), 16),
                    path);
      return;
    }
    for (const auto* field : *object.fields()) {
      if (field->deprecated()) continue;
      const auto field_path =
          field->name()->string_view() == "id" &&
                  object.name()->string_view() == "tsugumi.Artifact"
              ? path
              : path + "." + field->name()->str();
      const auto& type = *field->type();
      if (type.base_type() == reflection::Obj) {
        const auto& child = *schema.objects()->Get(type.index());
        if (object.is_struct()) {
          self(self, child, address + field->offset(), field_path);
        } else if (child.is_struct()) {
          self(self, child, table->GetAddressOf(field->offset()), field_path);
        } else {
          self(self, child, table->GetPointer<const uint8_t*>(field->offset()),
               field_path);
        }
      } else if (type.base_type() == reflection::Array &&
                 type.element() == reflection::Obj) {
        const auto& child = *schema.objects()->Get(type.index());
        for (size_t i = 0; i < type.fixed_length(); ++i)
          self(self, child, address + field->offset() + i * child.bytesize(),
               field_path + "[" + std::to_string(i) + "]");
      } else if (type.base_type() == reflection::Vector &&
                 type.element() == reflection::Obj) {
        const auto* vector = flatbuffers::GetFieldAnyV(*table, *field);
        if (!vector) continue;
        const auto& child = *schema.objects()->Get(type.index());
        for (size_t i = 0; i < vector->size(); ++i)
          self(self, child,
               child.is_struct()
                   ? flatbuffers::GetAnyVectorElemAddressOf<const uint8_t>(
                         vector, i, child.bytesize())
                   : flatbuffers::GetAnyVectorElemPointer<const uint8_t>(vector,
                                                                         i),
               field_path + "[" + std::to_string(i) + "]");
      } else if (type.base_type() == reflection::Union) {
        const auto* tag = object.fields()->LookupByKey(
            (field->name()->str() + "_type").c_str());
        if (!tag) continue;
        const auto* member =
            schema.enums()
                ->Get(type.index())
                ->values()
                ->LookupByKey(flatbuffers::GetFieldI<uint8_t>(*table, *tag));
        if (!member || !member->union_type() ||
            member->union_type()->base_type() != reflection::Obj)
          continue;
        self(self, *schema.objects()->Get(member->union_type()->index()),
             table->GetPointer<const uint8_t*>(field->offset()), field_path);
      }
    }
  };
  visit(visit, root,
        reinterpret_cast<const uint8_t*>(
            flatbuffers::GetRoot<flatbuffers::Table>(data)),
        "");
  return paths;
}

auto recorded_artifact_paths(const std::string& bfbs, const std::string& bytes)
    -> ArtifactValuePaths {
  const auto* schema_data = reinterpret_cast<const uint8_t*>(bfbs.data());
  flatbuffers::Verifier verifier(schema_data, bfbs.size());
  if (!reflection::VerifySchemaBuffer(verifier)) return {};
  const auto& schema = *reflection::GetSchema(schema_data);
  const auto* root = schema.root_table();
  const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
  if (!root || !flatbuffers::Verify(schema, *root, data, bytes.size()))
    return {};
  return value_artifact_paths(schema, *root, data);
}

}  // namespace

auto History::output_artifact_ids(const types::Type& type,
                                  std::string_view bytes)
    -> std::set<ArtifactKey> {
  const auto& schema = *type.schema();
  if (bytes.size() < sizeof(flatbuffers::uoffset_t) &&
      schema.objects()->LookupByKey("tsugumi.ArtifactId"))
    throw Failed(FailureKind::output_invalid,
                 "cannot read artifact UUIDs from a truncated output");
  std::set<ArtifactKey> ids;
  for (const auto& [id, _] :
       value_artifact_paths(schema, *type.root(),
                            reinterpret_cast<const uint8_t*>(bytes.data())))
    ids.insert(id);
  return ids;
}

auto History::output_artifacts(std::span<const Value> values)
    -> std::vector<OutputArtifactData> {
  std::vector<OutputArtifactData> outputs;
  for (const auto& value : values) {
    if (!value.type) continue;
    const auto& schema = *value.type->schema();
    if (!schema.objects()->LookupByKey("tsugumi.ArtifactId")) continue;
    const std::string_view bytes = value.bytes;
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    if (!flatbuffers::Verify(schema, *value.type->root(), data, bytes.size()))
      continue;
    for (const auto& [id, path] :
         value_artifact_paths(schema, *value.type->root(), data))
      outputs.push_back({id, value.name, path});
  }
  return outputs;
}

auto History::artifact_outputs(sql::Db& db, const ArtifactData& a)
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> outputs;
  const auto inv = invocation(db, a.run_id, a.card, a.invocation);
  if (!inv) return outputs;
  for (const auto& artifact : inv->summary.artifact_outputs)
    if (artifact.id == a.id)
      outputs.emplace_back(artifact.port, artifact.value_path);
  if (!inv->summary.artifact_outputs.empty() || !inv->record_io) return outputs;

  auto st = db.prepare(
      "SELECT v.port, s.bfbs, v.bytes FROM ivalues v JOIN schemas s ON "
      "s.hash=v.schema_hash WHERE v.run_id=? AND v.card=? AND "
      "v.invocation=? AND v.side=1 AND v.bytes IS NOT NULL ORDER BY v.port");
  st.bind(1, a.run_id).bind(2, key(a.card)).bind(3, a.invocation);
  while (st.step())
    for (const auto& [id, path] :
         recorded_artifact_paths(st.blob(1), st.blob(2)))
      if (id == a.id) outputs.emplace_back(st.text(0), path);
  return outputs;
}

std::vector<StoredValueMeta> History::value_metas(sql::Db& db,
                                                  const std::string& run_id,
                                                  const CardPath& card,
                                                  uint64_t n, bool input) {
  auto st = db.prepare(
      "SELECT v.port, s.bfbs, v.root_type, v.size, v.bytes, v.hash "
      "FROM ivalues v "
      "LEFT JOIN schemas s ON s.hash=v.schema_hash WHERE run_id=? AND "
      "card=? AND invocation=? AND side=? "
      "ORDER BY v.port");
  st.bind(1, run_id).bind(2, key(card)).bind(3, n).bind(4, input ? 0 : 1);
  std::vector<StoredValueMeta> out;
  while (st.step()) {
    StoredValueMeta m;
    m.name = st.text(0);
    if (!st.is_null(1)) m.bfbs = st.blob(1);
    m.root_type = st.text(2);
    m.size_bytes = st.u64(3);
    if (!st.is_null(4) && m.bfbs.find("tsugumi.ArtifactId") != std::string::npos) {
      std::set<ArtifactKey> ids;
      for (const auto& [id, _] : recorded_artifact_paths(m.bfbs, st.blob(4)))
        ids.insert(id);
      for (const auto& id : ids) {
        const auto record = artifact(db, id);
        const bool available = record && record->sealed_ms;
        m.artifacts.push_back(
            {id, available ? record->size_bytes : std::nullopt,
             record ? std::to_string(record->sequence) + "." +
                          std::to_string(record->counter)
                    : "",
             available ? Tsugumi::Data::ArtifactState::available
                       : Tsugumi::Data::ArtifactState::unavailable});
      }
    }
    m.hash = st.text(5);
    out.push_back(std::move(m));
  }
  return out;
}

std::optional<History::ValuePage> History::value_page(
    sql::Db& db, const std::string& run_id, const CardPath& card, uint64_t n,
    bool input, const std::string& port, uint64_t offset, uint64_t limit) {
  auto st = db.prepare(
      "SELECT substr(bytes, ?, ?), size, hash FROM ivalues "
      "WHERE run_id=? AND card=? AND invocation=? AND side=? AND port=?");
  st.bind(1, offset + 1)
      .bind(2, limit)
      .bind(3, run_id)
      .bind(4, key(card))
      .bind(5, n)
      .bind(6, input ? 0 : 1)
      .bind(7, port);
  if (!st.step()) return std::nullopt;
  ValuePage p;
  if (!st.is_null(0)) p.bytes = st.blob(0);
  p.size = static_cast<uint64_t>(st.i64(1));
  p.hash = st.text(2);
  return p;
}

std::optional<std::string> History::value_bytes(sql::Db& db,
                                                const std::string& run_id,
                                                const CardPath& card,
                                                uint64_t n, bool input,
                                                const std::string& port) {
  auto st = db.prepare(
      "SELECT bytes FROM ivalues WHERE run_id=? AND card=? "
      "AND invocation=? AND side=? AND port=?");
  st.bind(1, run_id)
      .bind(2, key(card))
      .bind(3, n)
      .bind(4, input ? 0 : 1)
      .bind(5, port);
  if (!st.step()) return std::nullopt;
  return st.blob(0);
}

auto History::put_last_port_input(sql::Db& db, const CardPath& card,
                                  const std::string& run_id,
                                  uint64_t activation,
                                  const SavedInputGroup& group) -> void {
  // Stored beside the per-Card rows under "<card>#<port>" ('#' never appears in
  // ids).
  CardPath k = card;
  k.back() += "#" + group.port;
  put_last_input(db, k, run_id, activation, {group});
}

std::optional<History::LastInput> History::last_port_input(
    sql::Db& db, const CardPath& card, const std::string& port) {
  CardPath k = card;
  k.back() += "#" + port;
  return last_input(db, k);
}

namespace {
std::string live_key(const InvocationSummaryData& s) {
  return s.run_id + "|" + key(s.card) + "|" + std::to_string(s.invocation);
}
}  // namespace

void History::set_live(const InvocationSummaryData& s) {
  std::lock_guard<std::mutex> l(live_mu_);
  live_[live_key(s)] = s;
}

void History::clear_live(const InvocationSummaryData& s) {
  std::lock_guard<std::mutex> l(live_mu_);
  live_.erase(live_key(s));
  last_output_.erase(live_key(s));
}

void History::note_output(const std::string& run_id, const CardPath& card,
                          uint64_t invocation) {
  const auto at = now_ms();
  std::lock_guard<std::mutex> l(live_mu_);
  last_output_[run_id + "|" + key(card) + "|" + std::to_string(invocation)] =
      at;
}

auto History::with_output(InvocationSummaryData s) -> InvocationSummaryData {
  std::lock_guard<std::mutex> l(live_mu_);
  if (const auto it = last_output_.find(live_key(s)); it != last_output_.end())
    s.last_output_at = it->second;
  return s;
}

std::vector<InvocationSummaryData> History::live(const std::string& run_id) {
  std::lock_guard<std::mutex> l(live_mu_);
  std::vector<InvocationSummaryData> out;
  for (const auto& [k, s] : live_)
    if (s.run_id == run_id) {
      out.push_back(s);
      if (const auto it = last_output_.find(k); it != last_output_.end())
        out.back().last_output_at = it->second;
    }
  return out;
}

void History::overlay(InvocationSummaryData& s) {
  if (s.state != CardState::running && s.state != CardState::preparing) return;
  std::lock_guard<std::mutex> l(live_mu_);
  if (auto it = live_.find(live_key(s)); it != live_.end()) {
    const auto truncated_at = s.log_truncated_at_bytes;
    s = it->second;
    s.log_truncated_at_bytes = truncated_at;
    if (const auto at = last_output_.find(it->first); at != last_output_.end())
      s.last_output_at = at->second;
  }
}

void History::update_detail(sql::Db& db, const InvocationSummaryData& s) {
  db.prepare(
        "UPDATE invocations SET detail=? WHERE run_id=? AND card=? AND "
        "invocation=?")
      .bind_blob(1, detail_bytes(s))
      .bind(2, s.run_id)
      .bind(3, key(s.card))
      .bind(4, s.invocation)
      .run();
}

std::optional<History::LastInput> History::last_input(sql::Db& db,
                                                      const CardPath& card) {
  auto st = db.prepare(
      "SELECT run_id, invocation, inputs FROM last_inputs WHERE card=?");
  st.bind(1, key(card));
  if (!st.step()) return std::nullopt;
  LastInput li;
  li.run_id = st.text(0);
  li.invocation = st.u64(1);
  const auto list = flat::decode<S::SavedInputs>(st.blob(2));
  if (!list) return std::nullopt;
  std::map<std::pair<std::string, std::string>, types::TypeRef> loaded_types;
  for (const auto& entry : list->groups) {
    SavedInputGroup group{
        entry->port, entry->execution_list, {}, entry->partial_result};
    for (const auto& value : entry->values) {
      Value v;
      v.name = group.port;
      v.schema = value->schema_id;
      v.bytes.assign(value->data.begin(), value->data.end());
      if (value->schema.empty()) {
        group.values.push_back(std::move(v));
        continue;
      }
      auto [type, inserted] = loaded_types.try_emplace(
          std::make_pair(value->schema, value->root_type));
      if (inserted) {
        auto schema_row = db.prepare("SELECT bfbs FROM schemas WHERE hash=?");
        schema_row.bind(1, value->schema);
        if (!schema_row.step()) return std::nullopt;
        auto decoded = std::make_shared<types::Type>();
        decoded->bfbs = schema_row.blob(0);
        decoded->hash = value->schema;
        decoded->root_type = value->root_type;
        type->second = std::move(decoded);
      }
      v.type = type->second;
      group.values.push_back(std::move(v));
    }
    li.groups.push_back(std::move(group));
  }
  return li;
}

namespace {
constexpr char kArtifactSelect[] =
    "SELECT id, run_id, card, invocation, sequence, counter, keep, location, "
    "created_ms, sealed_ms, size_bytes, tree_hash FROM artifacts ";

ArtifactData artifact_row(const sql::Stmt& st) {
  ArtifactData a;
  a.id = st.blob(0);
  a.run_id = st.text(1);
  a.card = unkey(st.text(2));
  a.invocation = st.u64(3);
  a.sequence = st.u64(4);
  a.counter = st.u64(5);
  a.keep = st.i64(6) != 0;
  a.location = st.text(7);
  a.created_ms = st.i64(8);
  if (!st.is_null(9)) a.sealed_ms = st.i64(9);
  if (!st.is_null(10)) a.size_bytes = st.u64(10);
  if (!st.is_null(11)) a.tree_hash = st.text(11);
  return a;
}
}  // namespace

std::optional<ArtifactData> History::artifact(sql::Db& db,
                                              const std::string& artifact_id) {
  auto st = db.prepare(std::string(kArtifactSelect) + "WHERE id=?");
  st.bind_blob(1, artifact_id);
  if (!st.step()) return std::nullopt;
  return artifact_row(st);
}

auto History::transient_artifacts(sql::Db& db, const std::string& run_id)
    -> std::vector<ArtifactData> {
  auto st =
      db.prepare(std::string(kArtifactSelect) + "WHERE run_id=? AND keep=0");
  st.bind(1, run_id);
  std::vector<ArtifactData> rows;
  while (st.step()) rows.push_back(artifact_row(st));
  return rows;
}

std::vector<ArtifactData> History::artifacts(
    sql::Db& db, const std::string& run_id, const std::optional<CardPath>& card,
    std::optional<uint64_t> invocation, const std::string& after,
    uint64_t limit) {
  auto st =
      db.prepare(std::string(kArtifactSelect) +
                 "WHERE run_id=?1 AND sealed_ms IS NOT NULL "
                 "AND (?2 IS NULL OR card=?2) AND (?3 IS NULL "
                 "OR invocation=?3) AND "
                 "(card || '\x1f' || printf('%020d', invocation) || '\x1f' || "
                 "printf('%020d', counter)) > ?4 "
                 "ORDER BY card, invocation, counter LIMIT ?5");
  st.bind(1, run_id);
  if (card)
    st.bind(2, key(*card));
  else
    st.bind_null(2);
  if (invocation)
    st.bind(3, *invocation);
  else
    st.bind_null(3);
  st.bind(4, after).bind(5, limit);
  std::vector<ArtifactData> out;
  while (st.step()) out.push_back(artifact_row(st));
  return out;
}

std::vector<RunSummaryData> History::runs_page(sql::Db& db,
                                               std::optional<CardPath> card,
                                               bool artifacts_only,
                                               int64_t below_seq,
                                               int64_t max_seq, uint64_t limit,
                                               std::vector<int64_t>* seqs) {
  std::string q = std::string("SELECT ") + kRunCols +
                  ", seq FROM runs WHERE seq<=?1 AND seq<?2 AND (?3=0 OR "
                  "EXISTS(SELECT 1 FROM artifacts a WHERE a.run_id=runs.run_id "
                  "AND a.sealed_ms IS NOT NULL AND (?4 IS NULL OR a.card=?4))) AND "
                  "(?4 IS NULL OR target=?4 OR EXISTS(SELECT 1 FROM run_cards "
                  "rc WHERE rc.run_id=runs.run_id AND rc.card=?4)) "
                  "ORDER BY seq DESC LIMIT ?5";
  auto st = db.prepare(q);
  st.bind(1, max_seq).bind(2, below_seq).bind(3, artifacts_only);
  if (card)
    st.bind(4, key(*card));
  else
    st.bind_null(4);
  st.bind(5, limit);
  std::vector<RunSummaryData> out;
  while (st.step()) {
    out.push_back(run_row(st));
    if (seqs)
      seqs->push_back(
          st.i64(14));  // after kRunCols, including both partial-run flags
  }
  return out;
}

int64_t History::max_run_seq(sql::Db& db) {
  auto st = db.prepare("SELECT COALESCE(MAX(seq), 0) FROM runs");
  st.step();
  return st.i64(0);
}

std::vector<std::string> History::unfinished_runs(sql::Db& db) {
  auto st = db.prepare("SELECT run_id FROM runs WHERE state IN (?,?,?,?)");
  st.bind(1, static_cast<int>(RunState::queued))
      .bind(2, static_cast<int>(RunState::preparing))
      .bind(3, static_cast<int>(RunState::running))
      .bind(4, static_cast<int>(RunState::cancelling));
  std::vector<std::string> out;
  while (st.step()) out.push_back(st.text(0));
  return out;
}

LogRead History::read_log(sql::Db& db, const std::string& run_id,
                          const std::optional<CardPath>& card,
                          std::optional<uint64_t> invocation, LogStream stream,
                          uint64_t offset, bool tail, uint64_t limit,
                          std::optional<uint64_t> before, bool terminal,
                          std::optional<uint64_t> since) {
  int s = static_cast<int>(stream);
  const char* col = !card ? "run_off" : (invocation ? "inv_off" : "card_off");
  const char* lf_col = !card ? "run_lf" : (invocation ? "inv_lf" : "card_lf");
  std::string where = "run_id=?1 AND stream=?2";
  if (card) where += " AND card=?3";
  if (card && invocation) where += " AND invocation=?4";
  auto bind = [&](sql::Stmt& st) {
    st.bind(1, run_id).bind(2, s);
    if (card) st.bind(3, key(*card));
    if (card && invocation) st.bind(4, *invocation);
  };
  LogRead out;
  if (card && invocation) {
    const bool build = stream == LogStream::build || stream == LogStream::journal;
    out.log_truncated_at_bytes =
        log_truncated_at(db, run_id, *card, *invocation, build);
    if (!out.log_truncated_at_bytes && stream == LogStream::journal)
      out.log_truncated_at_bytes = log_truncated_at(db, run_id, *card, *invocation);
  }
  {
    auto st = db.prepare(std::string("SELECT ") + col +
                         " + length(data) FROM logs WHERE " + where +
                         " ORDER BY " + col + " DESC LIMIT 1");
    bind(st);
    out.exists = st.step();
    if (out.exists) out.size = st.u64(0);
  }
  if (terminal)
    return read_terminal(
        db, {run_id, card ? key(*card) : "", invocation.value_or(0), s},
        std::move(out), offset, tail, limit, before, since);
  const uint64_t edge = tail ? out.size : before.value_or(offset);
  if (edge > out.size)
    throw Failed(FailureKind::invalid_range,
                 "offset is past the end of the log");
  if (tail || before) offset = edge > limit ? edge - limit : 0;
  const uint64_t stop_at =
      (tail || before) ? edge : std::min(out.size, offset + limit);
  // Include the preceding byte to identify line starts, and the next byte
  // to check the exclusive end's UTF-8 boundary.
  const uint64_t scan_from = offset == 0 ? 0 : offset - 1;
  const uint64_t scan_end = std::min(out.size, stop_at + 1);
  std::string buf;
  uint64_t buf_start = offset;
  {
    auto st = db.prepare(std::string("SELECT ") + col + ", data, " + lf_col +
                         " FROM logs WHERE " + where + " AND " + col +
                         " >= (SELECT " + col + " FROM logs WHERE " + where +
                         " AND " + col + " <= ?5 ORDER BY " + col +
                         " DESC LIMIT 1) AND " + col + " < ?6 ORDER BY " + col);
    bind(st);
    st.bind(5, scan_from).bind(6, scan_end);
    bool first = true;
    while (st.step()) {
      uint64_t o = st.u64(0);
      std::string d = st.blob(1);
      if (first) {
        buf_start = o;
        out.line_number += st.u64(2);
        first = false;
      }
      if (o != buf_start + buf.size())
        throw Failed(FailureKind::persistence_failed,
                     "discontinuous log chunks");
      buf += d;
    }
  }
  if (scan_from < scan_end &&
      (buf_start > scan_from || buf_start + buf.size() < scan_end))
    throw Failed(FailureKind::persistence_failed, "discontinuous log chunks");
  auto at = [&](uint64_t pos) -> int {
    if (pos < buf_start || pos - buf_start >= buf.size()) return -1;
    return static_cast<unsigned char>(buf[pos - buf_start]);
  };
  auto inside_character = [&](uint64_t pos) {
    const auto byte = at(pos);
    return byte >= 0 && !char_start(static_cast<unsigned char>(byte));
  };
  uint64_t stop = stop_at;
  if (tail || before) {
    while (offset < stop && inside_character(offset)) ++offset;
    if (before && inside_character(stop))
      throw Failed(FailureKind::invalid_range,
                   "before is not on a UTF-8 character boundary");
  } else {
    if (inside_character(offset))
      throw Failed(FailureKind::invalid_range,
                   "offset is not on a UTF-8 character boundary");
    while (stop > offset && inside_character(stop)) --stop;
  }
  const auto prefix = offset - buf_start;
  out.line_number += std::count(buf.begin(), buf.begin() + prefix, '\n');
  out.starts_line = offset == 0 || at(offset - 1) == '\n';
  out.text = buf.substr(prefix, stop - offset);
  out.offset = offset;
  out.next_offset = stop;
  out.truncated = ((tail || before) && offset > 0) || stop < out.size;
  return out;
}

std::string History::make_cursor(int64_t hi, const std::string& after) const {
  S::CursorT c;
  c.hi = hi;
  c.after = after;
  return crypto::base64(flat::encode<S::Cursor>(c));
}

std::pair<int64_t, std::string> History::open_cursor(
    const std::string& cursor) const {
  std::string raw;
  const auto c = crypto::base64_decode(cursor, raw)
                     ? flat::decode<S::Cursor>(raw)
                     : std::nullopt;
  if (!c) throw Failed(FailureKind::invalid_cursor, "malformed cursor");
  return {c->hi, c->after};
}

auto History::delete_category(sql::Db& db, const std::string& run_id,
                              R::RunRecordCategory category) -> void {
  switch (category) {
    case R::RunRecordCategory::Logs:
      for (const auto* table : {"logs", "log_terminal_ends", "log_terminal_tiles"})
        db.prepare(std::string("DELETE FROM ") + table +
                   " WHERE run_id=? AND stream=?")
            .bind(1, run_id)
            .bind(2, static_cast<int>(LogStream::output))
            .run();
      db.prepare("DELETE FROM log_truncations WHERE run_id=? AND invocation>0")
          .bind(1, run_id).run();
      break;
    case R::RunRecordCategory::Values:
      db.prepare("DELETE FROM ivalues WHERE run_id=?").bind(1, run_id).run();
      break;
    case R::RunRecordCategory::Artifacts:
      db.prepare("DELETE FROM artifacts WHERE run_id=?").bind(1, run_id).run();
      break;
    default:
      throw sql::Error("invalid run record category");
  }
}

// Delete exactly the finished runs selected by the person, in the primary
// database.
std::vector<std::string> History::delete_runs(
    std::optional<int64_t> ended_before,
    const std::optional<std::vector<std::string>>& run_ids) {
  std::vector<std::string> ids;
  write_sync([&](sql::Db& db) {
    std::vector<std::string> selected_ids;
    db.exec(
        "CREATE TEMP TABLE IF NOT EXISTS deleting_runs(run_id TEXT PRIMARY "
        "KEY); DELETE FROM temp.deleting_runs");
    const auto states =
        std::to_string(static_cast<int>(RunState::succeeded)) + "," +
        std::to_string(static_cast<int>(RunState::failed)) + "," +
        std::to_string(static_cast<int>(RunState::cancelled));
    std::string candidates = "runs";
    if (run_ids) {
      db.exec(
          "CREATE TEMP TABLE IF NOT EXISTS requested_runs(run_id TEXT PRIMARY "
          "KEY); DELETE FROM temp.requested_runs");
      auto insert = db.prepare(
          "INSERT OR IGNORE INTO temp.requested_runs VALUES (?)");
      for (const auto& id : *run_ids) {
        insert.reset();
        insert.bind(1, id).run();
      }
      candidates =
          "temp.requested_runs CROSS JOIN runs USING (run_id)";
    }
    auto select = db.prepare(
        "INSERT INTO temp.deleting_runs SELECT runs.run_id FROM " +
        candidates + " WHERE state IN (" + states + ")" +
        (ended_before ? " AND ended_at < ?1" : "") +
        " AND runs.run_id NOT IN (SELECT run_id FROM remote_jobs)");
    if (ended_before) select.bind(1, *ended_before);
    select.run();
    auto selected = db.prepare("SELECT run_id FROM temp.deleting_runs");
    while (selected.step()) selected_ids.push_back(selected.text(0));
    // Saved inputs intentionally outlive deleted runs; schema objects may still
    // be needed by them.
    for (const auto* table :
         {"runs", "run_cards", "run_batches", "build_environments",
          "environment_paths", "run_substituters", "run_commits", "invocations",
          "ivalues", "logs", "log_terminal_ends", "log_terminal_tiles",
          "log_truncations", "artifacts", "statuses", "invocation_sequences",
          "active_environments", "run_definition_paths"})
      db.exec(std::string("DELETE FROM ") + table +
              " WHERE run_id IN (SELECT run_id FROM temp.deleting_runs)");
    ids = std::move(selected_ids);
  });
  return ids;
}

}  // namespace tsugumi
