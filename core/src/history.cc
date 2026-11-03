#include "history.hh"

#include "util/crypto.hh"
#include "schema.hh"
#include "util/log.hh"
#include "wire.hh"

namespace tsugumi {

namespace {

std::string key(const CardPath& p) { return path_key(p); }
CardPath unkey(const std::string& s) { return path_from_key(s); }

std::string waiting_json(const std::vector<WaitingInputData>& w) {
  schema::WaitingList list;
  for (const auto& wi : w) {
    schema::WaitingJson j;
    *j.port = wi.port;
    for (const auto& u : wi.upstream) {
      schema::UpstreamJson uj;
      *uj.card = key(u.card);
      *uj.port = u.port;
      j.upstream->push_back(std::move(uj));
    }
    list.items->push_back(std::move(j));
  }
  return schema::encode(list);
}

std::vector<WaitingInputData> waiting_from(const std::string& s) {
  std::vector<WaitingInputData> out;
  schema::WaitingList list;
  if (s.empty() || schema::decode(s, list)) return out;
  for (const auto& wi : *list.items) {
    WaitingInputData w;
    w.port = *wi.port;
    for (const auto& u : *wi.upstream) w.upstream.push_back({unkey(*u.card), *u.port});
    out.push_back(std::move(w));
  }
  return out;
}

std::string detail_json(const InvocationSummaryData& s) {
  schema::InvocationDetailJson d;
  for (const auto& st : s.stages) {
    schema::StageJson j;
    *j.phase = static_cast<int>(st.phase);
    *j.outcome = static_cast<int>(st.outcome);
    *j.start = st.start;
    *j.end = st.end.value_or(-1);
    d.stages->push_back(std::move(j));
  }
  *d.worker_reused = s.worker_reused ? (*s.worker_reused ? 1 : 0) : -1;
  for (const auto& p : s.outputs_sent) d.outputs_sent->push_back(p);
  *d.ready_at = s.ready_at.value_or(-1);
  return schema::encode(d);
}

void detail_into(const std::string& text, InvocationSummaryData& s) {
  schema::InvocationDetailJson d;
  if (text.empty() || schema::decode(text, d)) return;
  for (const auto& j : *d.stages) {
    StageTimeData st;
    st.phase = static_cast<Phase>(*j.phase);
    st.outcome = static_cast<StageOutcome>(*j.outcome);
    st.start = *j.start;
    if (*j.end >= 0) st.end = *j.end;
    s.stages.push_back(st);
  }
  if (*d.worker_reused >= 0) s.worker_reused = *d.worker_reused == 1;
  for (const auto& p : *d.outputs_sent) s.outputs_sent.push_back(p);
  if (*d.ready_at >= 0) s.ready_at = *d.ready_at;
}

std::string sources_json(const std::vector<InputSourceData>& s) {
  schema::InputSourceList list;
  for (const auto& x : s) {
    schema::InputSourceJson j;
    *j.port = x.port;
    *j.card = key(x.card);
    *j.invocation = x.invocation;
    list.items->push_back(std::move(j));
  }
  return schema::encode(list);
}

std::vector<InputSourceData> sources_from(const std::string& s) {
  std::vector<InputSourceData> out;
  schema::InputSourceList list;
  if (s.empty() || schema::decode(s, list)) return out;
  for (const auto& x : *list.items) out.push_back({*x.port, unkey(*x.card), *x.invocation});
  return out;
}

void bind_failure(sql::Stmt& st, int i, const std::optional<FailureData>& f) {
  if (f) st.bind_blob(i, f->to_bytes());
  else st.bind_null(i);
}

std::optional<FailureData> col_failure(const sql::Stmt& st, int c) {
  if (st.is_null(c)) return std::nullopt;
  return FailureData::from_bytes(st.blob(c));
}

std::string put_schema(sql::Db& db, const types::Type& t) {
  // Types do not change: their hash is computed once, where they are made.
  std::string h = t.hash.empty() ? crypto::sha256_hex(t.bfbs) : t.hash;
  db.prepare("INSERT OR IGNORE INTO schemas(hash, root_type, bfbs) VALUES(?,?,?)")
      .bind(1, h).bind(2, t.root_type).bind_blob(3, t.bfbs).run();
  return h;
}

RunSummaryData run_row(const sql::Stmt& st) {
  // Columns: run_id, state, rev, execution_rev, accepted_at, started_at, ended_at,
  //          failure, source_hash, target, propagate
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
  return s;
}

constexpr const char* kRunCols =
    "run_id, state, rev, execution_rev, accepted_at, started_at, ended_at, failure, source_hash, "
    "target, propagate";

InvocationSummaryData inv_row(const sql::Stmt& st, const std::string& run_id, int cached_col = -1,
                              int detail_col = -1) {
  // Columns: card, invocation, state, phase, started_at, ended_at, duration_ms, failure, waiting
  InvocationSummaryData s;
  s.run_id = run_id;
  s.card = unkey(st.text(0));
  s.invocation = st.u64(1);
  s.state = static_cast<CardState>(st.i64(2));
  if (!st.is_null(3)) s.phase = static_cast<Phase>(st.i64(3));
  s.started_at = st.opt_i64(4);
  s.ended_at = st.opt_i64(5);
  s.duration_ms = st.opt_u64(6);
  s.failure = col_failure(st, 7);
  s.waiting = waiting_from(st.text(8));
  if (cached_col >= 0 && !st.is_null(cached_col)) s.build_cached = st.i64(cached_col) != 0;
  if (detail_col >= 0 && !st.is_null(detail_col)) detail_into(st.text(detail_col), s);
  return s;
}

bool char_start(unsigned char c) { return (c & 0xC0) != 0x80; }

}  // namespace

History::History(const std::filesystem::path& db_path, std::string cursor_secret, std::string fault)
    : path_(db_path), secret_(std::move(cursor_secret)), fault_(std::move(fault)) {
  writer_db_ = std::make_unique<sql::Db>(path_.string());
  // Single process owns the database (guarded by the project lock).
  writer_db_->exec("PRAGMA journal_mode=WAL");
  writer_db_->exec("PRAGMA synchronous=FULL");
  writer_db_->exec("PRAGMA foreign_keys=ON");
  init_schema(*writer_db_);
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
  db.exec(R"SQL(
CREATE TABLE IF NOT EXISTS runs(
  seq INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id TEXT NOT NULL UNIQUE, op_id TEXT NOT NULL UNIQUE,
  state INTEGER NOT NULL, rev INTEGER, execution_rev INTEGER,
  accepted_at INTEGER, started_at INTEGER, ended_at INTEGER, failure BLOB,
  source_hash TEXT NOT NULL, target TEXT NOT NULL, propagate INTEGER NOT NULL,
  has_artifacts INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS run_cards(run_id TEXT NOT NULL, card TEXT NOT NULL, PRIMARY KEY(run_id, card));
CREATE TABLE IF NOT EXISTS invocations(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL,
  state INTEGER NOT NULL, phase INTEGER, started_at INTEGER, ended_at INTEGER,
  duration_ms INTEGER, failure BLOB, waiting TEXT, record_io INTEGER NOT NULL,
  input_sources TEXT, display BLOB,
  PRIMARY KEY(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS statuses(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL,
  state INTEGER NOT NULL, phase INTEGER, started_at INTEGER, failure BLOB, waiting TEXT,
  updated_at INTEGER NOT NULL,
  PRIMARY KEY(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS schemas(hash TEXT PRIMARY KEY, root_type TEXT NOT NULL, bfbs BLOB NOT NULL);
CREATE TABLE IF NOT EXISTS ivalues(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL, side INTEGER NOT NULL,
  port TEXT NOT NULL, kind INTEGER NOT NULL, schema_hash TEXT NOT NULL, root_type TEXT NOT NULL,
  size INTEGER NOT NULL, bytes BLOB, artifact_id TEXT,
  PRIMARY KEY(run_id, card, invocation, side, port));
CREATE TABLE IF NOT EXISTS last_inputs(
  card TEXT PRIMARY KEY, run_id TEXT NOT NULL, invocation INTEGER NOT NULL, inputs TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS logs(
  id INTEGER PRIMARY KEY AUTOINCREMENT, run_id TEXT NOT NULL, card TEXT NOT NULL,
  invocation INTEGER NOT NULL, stream INTEGER NOT NULL,
  run_off INTEGER NOT NULL, card_off INTEGER NOT NULL, inv_off INTEGER NOT NULL, data BLOB NOT NULL);
CREATE INDEX IF NOT EXISTS logs_run ON logs(run_id, stream, run_off);
CREATE INDEX IF NOT EXISTS logs_card ON logs(run_id, card, stream, card_off);
CREATE INDEX IF NOT EXISTS logs_inv ON logs(run_id, card, invocation, stream, inv_off);
CREATE TABLE IF NOT EXISTS artifacts(
  artifact_id TEXT PRIMARY KEY, run_id TEXT NOT NULL, card TEXT NOT NULL,
  invocation INTEGER NOT NULL, output TEXT NOT NULL, kind INTEGER NOT NULL, name TEXT NOT NULL,
  members TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS artifacts_run ON artifacts(run_id, card, invocation);
CREATE TABLE IF NOT EXISTS displays(
  run_id TEXT NOT NULL, card TEXT NOT NULL, revision INTEGER NOT NULL, data BLOB NOT NULL,
  PRIMARY KEY(run_id, card));
CREATE TABLE IF NOT EXISTS receipts(
  op_id TEXT PRIMARY KEY, kind TEXT NOT NULL, digest TEXT NOT NULL, result BLOB NOT NULL);
)SQL");
  // Columns added after the first release (older databases are migrated in place).
  bool has_cached = false, has_detail = false;
  {
    auto st = db.prepare("SELECT name FROM pragma_table_info('invocations')");
    while (st.step()) {
      has_cached |= st.text(0) == "build_cached";
      has_detail |= st.text(0) == "detail";
    }
  }
  if (!has_cached) db.exec("ALTER TABLE invocations ADD COLUMN build_cached INTEGER");
  if (!has_detail) db.exec("ALTER TABLE invocations ADD COLUMN detail TEXT");
}

sql::Db& History::reader() {
  std::lock_guard<std::mutex> l(rmu_);
  auto& slot = readers_[std::this_thread::get_id()];
  if (!slot) {
    slot = std::make_unique<sql::Db>(path_.string(), true);
    slot->exec("PRAGMA query_only=ON");
  }
  return *slot;
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

void History::write_sync(const Work& work) {
  std::mutex m;
  std::condition_variable cv;
  bool finished = false, ok = false;
  submit(work, [&](bool r) {
    std::lock_guard<std::mutex> l(m);
    ok = r;
    finished = true;
    cv.notify_one();
  });
  std::unique_lock<std::mutex> l(m);
  cv.wait(l, [&] { return finished; });
  if (!ok) throw Failed(FailureKind::persistence_failed, "history write failed");
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
      log::error(std::string("history batch failed, retrying individually: ") + e.what());
      ok = false;
    }
    if (!ok) {
      // Cached log offsets may include rolled back rows.
      log_ends_.clear();
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

void History::insert_run(sql::Db& db, const RunSummaryData& s, const std::string& op_id) {
  db.prepare("INSERT INTO runs(run_id, op_id, state, rev, execution_rev, accepted_at, started_at, "
             "ended_at, failure, source_hash, target, propagate) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)")
      .bind(1, *s.status.run_id).bind(2, op_id).bind(3, static_cast<int>(s.status.state))
      .bind(4, s.status.rev).bind(5, s.status.execution_rev).bind(6, s.status.accepted_at)
      .bind(7, s.status.started_at).bind(8, s.status.ended_at)
      .bind_null(9)
      .bind(10, s.source_hash).bind(11, key(s.target)).bind(12, s.propagate)
      .run();
  if (s.status.failure) update_run(db, s.status);
}

void History::update_run(sql::Db& db, const RunStatusData& s) {
  auto st = db.prepare("UPDATE runs SET state=?, started_at=?, ended_at=?, failure=? WHERE run_id=?");
  st.bind(1, static_cast<int>(s.state)).bind(2, s.started_at).bind(3, s.ended_at);
  bind_failure(st, 4, s.failure);
  st.bind(5, *s.run_id).run();
}

void History::put_status(sql::Db& db, const InvocationSummaryData& s) {
  auto st = db.prepare(
      "INSERT INTO statuses(run_id, card, invocation, state, phase, started_at, failure, waiting, updated_at) "
      "VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(run_id, card, invocation) DO UPDATE SET "
      "state=excluded.state, phase=excluded.phase, started_at=excluded.started_at, "
      "failure=excluded.failure, waiting=excluded.waiting, updated_at=excluded.updated_at");
  st.bind(1, s.run_id).bind(2, key(s.card)).bind(3, s.invocation).bind(4, static_cast<int>(s.state));
  if (s.phase) st.bind(5, static_cast<int>(*s.phase));
  else st.bind_null(5);
  st.bind(6, s.started_at);
  bind_failure(st, 7, s.failure);
  st.bind(8, waiting_json(s.waiting)).bind(9, now_ms()).run();
  db.prepare("INSERT OR IGNORE INTO run_cards(run_id, card) VALUES(?,?)")
      .bind(1, s.run_id).bind(2, key(s.card)).run();
}

void History::complete_invocation(sql::Db& db, const InvocationRecordData& r) const {
  if (fault_ == "history_invocation") throw sql::Error("injected fault: invocation record write failed");
  const auto& s = r.summary;
  auto st = db.prepare(
      "INSERT OR REPLACE INTO invocations(run_id, card, invocation, state, phase, started_at, ended_at, "
      "duration_ms, failure, waiting, record_io, input_sources, display, build_cached, detail) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  st.bind(1, s.run_id).bind(2, key(s.card)).bind(3, s.invocation).bind(4, static_cast<int>(s.state));
  if (s.phase) st.bind(5, static_cast<int>(*s.phase));
  else st.bind_null(5);
  st.bind(6, s.started_at).bind(7, s.ended_at).bind(8, s.duration_ms);
  bind_failure(st, 9, s.failure);
  st.bind(10, waiting_json(s.waiting)).bind(11, r.record_io).bind(12, sources_json(r.input_sources));
  if (r.display) st.bind_blob(13, wire::standalone_display(*r.display));
  else st.bind_null(13);
  if (s.build_cached) st.bind(14, *s.build_cached);
  else st.bind_null(14);
  st.bind(15, detail_json(s));
  st.run();
  // The completion removes exactly this invocation's progress status, never a newer one.
  db.prepare("DELETE FROM statuses WHERE run_id=? AND card=? AND invocation=?")
      .bind(1, s.run_id).bind(2, key(s.card)).bind(3, s.invocation).run();
  db.prepare("INSERT OR IGNORE INTO run_cards(run_id, card) VALUES(?,?)")
      .bind(1, s.run_id).bind(2, key(s.card)).run();
  if (r.record_io) {
    auto put = [&](const Value& v, int side) {
      std::string h = put_schema(db, *v.type);
      auto ins = db.prepare(
          "INSERT OR REPLACE INTO ivalues(run_id, card, invocation, side, port, kind, schema_hash, "
          "root_type, size, bytes, artifact_id) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
      ins.bind(1, s.run_id).bind(2, key(s.card)).bind(3, s.invocation).bind(4, side).bind(5, v.name)
          .bind(6, static_cast<int>(v.kind)).bind(7, h).bind(8, v.type->root_type)
          .bind(9, static_cast<uint64_t>(v.bytes.size())).bind_blob(10, v.bytes).bind(11, v.artifact_id)
          .run();
    };
    for (const auto& v : r.inputs) put(v, 0);
    for (const auto& v : r.outputs) put(v, 1);
  }
}

void History::put_last_input(sql::Db& db, const CardPath& card, const std::string& run_id,
                             uint64_t invocation, const std::vector<Value>& inputs) {
  schema::SavedInputList list;
  for (const auto& v : inputs) {
    schema::SavedInputJson j;
    *j.name = v.name;
    *j.kind = static_cast<int>(v.kind);
    *j.schema = put_schema(db, *v.type);
    *j.root_type = v.type->root_type;
    *j.data_b64 = crypto::base64(v.bytes);
    list.items->push_back(std::move(j));
  }
  db.prepare("INSERT OR REPLACE INTO last_inputs(card, run_id, invocation, inputs) VALUES(?,?,?,?)")
      .bind(1, key(card)).bind(2, run_id).bind(3, invocation).bind(4, schema::encode(list)).run();
}

void History::put_artifact(sql::Db& db, const ArtifactData& a) {
  schema::MemberList m;
  for (const auto& x : a.members) {
    schema::MemberJson j;
    *j.path = x.path;
    *j.size = x.size_bytes;
    *j.hash = x.hash;
    m.items->push_back(std::move(j));
  }
  db.prepare("INSERT INTO artifacts(artifact_id, run_id, card, invocation, output, kind, name, members) "
             "VALUES(?,?,?,?,?,?,?,?)")
      .bind(1, a.artifact_id).bind(2, a.run_id).bind(3, key(a.card)).bind(4, a.invocation)
      .bind(5, a.output).bind(6, static_cast<int>(a.kind)).bind(7, a.name)
      .bind(8, schema::encode(m)).run();
  db.prepare("UPDATE runs SET has_artifacts=1 WHERE run_id=?").bind(1, a.run_id).run();
}

void History::put_display(sql::Db& db, const DisplayData& d) {
  db.prepare("INSERT OR REPLACE INTO displays(run_id, card, revision, data) VALUES(?,?,?,?)")
      .bind(1, d.run_id).bind(2, key(d.card)).bind(3, d.revision)
      .bind_blob(4, wire::standalone_display(d)).run();
}

void History::put_receipt(sql::Db& db, const std::string& op_id, const std::string& kind,
                          const std::string& digest, const std::string& bytes) {
  db.prepare("INSERT OR REPLACE INTO receipts(op_id, kind, digest, result) VALUES(?,?,?,?)")
      .bind(1, op_id).bind(2, kind).bind(3, digest).bind_blob(4, bytes).run();
}

std::vector<LogEnd> History::append_log(sql::Db& db, const std::string& run_id, const CardPath& card,
                                        uint64_t invocation, LogStream stream, const std::string& text) {
  if (text.empty()) return {};
  int s = static_cast<int>(stream);
  std::string ck = key(card);
  std::string rk = "R|" + run_id + "|" + std::to_string(s);
  std::string kk = "C|" + run_id + "|" + ck + "|" + std::to_string(s);
  std::string ik = "I|" + run_id + "|" + ck + "|" + std::to_string(invocation) + "|" + std::to_string(s);
  auto load = [&](const std::string& k, const char* sql_text, bool with_card, bool with_inv) {
    auto it = log_ends_.find(k);
    if (it != log_ends_.end()) return it->second;
    auto st = db.prepare(sql_text);
    int i = 1;
    st.bind(i++, run_id);
    if (with_card) st.bind(i++, ck);
    if (with_inv) st.bind(i++, invocation);
    st.bind(i++, s);
    uint64_t v = st.step() && !st.is_null(0) ? st.u64(0) : 0;
    log_ends_[k] = v;
    return v;
  };
  uint64_t r0 = load(rk, "SELECT MAX(run_off + length(data)) FROM logs WHERE run_id=? AND stream=?", false, false);
  uint64_t c0 = load(kk, "SELECT MAX(card_off + length(data)) FROM logs WHERE run_id=? AND card=? AND stream=?", true, false);
  uint64_t i0 = load(ik, "SELECT MAX(inv_off + length(data)) FROM logs WHERE run_id=? AND card=? AND invocation=? AND stream=?", true, true);
  db.prepare("INSERT INTO logs(run_id, card, invocation, stream, run_off, card_off, inv_off, data) "
             "VALUES(?,?,?,?,?,?,?,?)")
      .bind(1, run_id).bind(2, ck).bind(3, invocation).bind(4, s).bind(5, r0).bind(6, c0).bind(7, i0)
      .bind_blob(8, text).run();
  log_ends_[rk] = r0 + text.size();
  log_ends_[kk] = c0 + text.size();
  log_ends_[ik] = i0 + text.size();
  return {{run_id, std::nullopt, std::nullopt, stream, r0 + text.size()},
          {run_id, card, invocation, stream, i0 + text.size()}};
}

std::vector<LogEnd> History::append_run_journal(sql::Db& db, const std::string& run_id,
                                                const std::string& text) {
  int s = static_cast<int>(LogStream::journal);
  std::string rk = "R|" + run_id + "|" + std::to_string(s);
  auto it = log_ends_.find(rk);
  uint64_t r0;
  if (it != log_ends_.end()) {
    r0 = it->second;
  } else {
    auto st = db.prepare("SELECT MAX(run_off + length(data)) FROM logs WHERE run_id=? AND stream=?");
    st.bind(1, run_id).bind(2, s);
    r0 = st.step() && !st.is_null(0) ? st.u64(0) : 0;
  }
  db.prepare("INSERT INTO logs(run_id, card, invocation, stream, run_off, card_off, inv_off, data) "
             "VALUES(?,?,?,?,?,0,0,?)")
      .bind(1, run_id).bind(2, "").bind(3, 0).bind(4, s).bind(5, r0).bind_blob(6, text).run();
  log_ends_[rk] = r0 + text.size();
  return {{run_id, std::nullopt, std::nullopt, LogStream::journal, r0 + text.size()}};
}

std::optional<RunSummaryData> History::run(sql::Db& db, const std::string& run_id) {
  auto st = db.prepare(std::string("SELECT ") + kRunCols + " FROM runs WHERE run_id=?");
  st.bind(1, run_id);
  if (!st.step()) return std::nullopt;
  return run_row(st);
}

std::optional<RunSummaryData> History::latest_run(sql::Db& db) {
  auto st = db.prepare(std::string("SELECT ") + kRunCols + " FROM runs ORDER BY seq DESC LIMIT 1");
  if (!st.step()) return std::nullopt;
  return run_row(st);
}

std::optional<History::ReceiptRow> History::receipt(sql::Db& db, const std::string& op_id) {
  auto st = db.prepare("SELECT kind, digest, result FROM receipts WHERE op_id=?");
  st.bind(1, op_id);
  if (!st.step()) return std::nullopt;
  return ReceiptRow{st.text(0), st.text(1), st.blob(2)};
}

std::optional<std::string> History::run_receipt_digest(sql::Db& db, const std::string& op_id) {
  auto r = receipt(db, op_id);
  if (!r) return std::nullopt;
  return r->digest;
}

std::vector<InvocationSummaryData> History::invocations(sql::Db& db, const std::string& run_id,
                                                        const CardPath& card, uint64_t after,
                                                        uint64_t limit) {
  // Completed records and in-progress statuses, ascending invocation number.
  auto st = db.prepare(
      "SELECT card, invocation, state, phase, started_at, ended_at, duration_ms, failure, waiting, build_cached, detail "
      "FROM invocations WHERE run_id=?1 AND card=?2 AND invocation>?3 "
      "UNION ALL SELECT card, invocation, state, phase, started_at, NULL, NULL, failure, waiting, NULL, NULL "
      "FROM statuses WHERE run_id=?1 AND card=?2 AND invocation>?3 AND invocation NOT IN "
      "(SELECT invocation FROM invocations WHERE run_id=?1 AND card=?2) "
      "ORDER BY invocation LIMIT ?4");
  st.bind(1, run_id).bind(2, key(card)).bind(3, after).bind(4, limit);
  std::vector<InvocationSummaryData> out;
  while (st.step()) out.push_back(inv_row(st, run_id, 9, 10));
  return out;
}

std::optional<InvocationRecordData> History::invocation(sql::Db& db, const std::string& run_id,
                                                        const CardPath& card, uint64_t n) {
  auto st = db.prepare(
      "SELECT card, invocation, state, phase, started_at, ended_at, duration_ms, failure, waiting, "
      "record_io, input_sources, display, build_cached, detail FROM invocations WHERE run_id=? AND card=? AND invocation=?");
  st.bind(1, run_id).bind(2, key(card)).bind(3, n);
  if (!st.step()) return std::nullopt;
  InvocationRecordData r;
  r.summary = inv_row(st, run_id, 12, 13);
  r.record_io = st.i64(9) != 0;
  r.input_sources = sources_from(st.text(10));
  if (!st.is_null(11)) r.display = wire::standalone_display(st.blob(11));
  return r;
}

std::optional<InvocationSummaryData> History::status(sql::Db& db, const std::string& run_id,
                                                     const CardPath& card, uint64_t n) {
  auto st = db.prepare(
      "SELECT card, invocation, state, phase, started_at, NULL, NULL, failure, waiting, updated_at "
      "FROM statuses WHERE run_id=? AND card=? AND invocation=?");
  st.bind(1, run_id).bind(2, key(card)).bind(3, n);
  if (!st.step()) return std::nullopt;
  return inv_row(st, run_id);
}

std::optional<uint64_t> History::latest_invocation(sql::Db& db, const std::string& run_id,
                                                   const CardPath& card, bool include_status) {
  std::string q = "SELECT MAX(invocation) FROM (SELECT invocation FROM invocations WHERE run_id=?1 AND card=?2";
  if (include_status) q += " UNION ALL SELECT invocation FROM statuses WHERE run_id=?1 AND card=?2";
  q += ")";
  auto st = db.prepare(q);
  st.bind(1, run_id).bind(2, key(card));
  if (!st.step() || st.is_null(0)) return std::nullopt;
  return st.u64(0);
}

std::optional<uint64_t> History::latest_completed(sql::Db& db, const std::string& run_id,
                                                  const CardPath& card) {
  return latest_invocation(db, run_id, card, false);
}

bool History::run_has_card(sql::Db& db, const std::string& run_id, const CardPath& card) {
  auto st = db.prepare("SELECT 1 FROM run_cards WHERE run_id=? AND card=? UNION ALL "
                       "SELECT 1 FROM runs WHERE run_id=? AND target=? LIMIT 1");
  st.bind(1, run_id).bind(2, key(card)).bind(3, run_id).bind(4, key(card));
  return st.step();
}

History::Aggregate History::aggregate(sql::Db& db, const std::string& run_id, const CardPath& card) {
  auto st = db.prepare(
      "SELECT COUNT(*), SUM(state=?3), SUM(state=?4), SUM(state=?5), COALESCE(SUM(duration_ms),0) "
      "FROM invocations WHERE run_id=?1 AND card=?2");
  st.bind(1, run_id).bind(2, key(card)).bind(3, static_cast<int>(CardState::succeeded))
      .bind(4, static_cast<int>(CardState::failed)).bind(5, static_cast<int>(CardState::cancelled));
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

std::optional<FailureData> History::latest_failure(sql::Db& db, const std::string& run_id,
                                                   const CardPath& card) {
  auto st = db.prepare("SELECT failure FROM invocations WHERE run_id=? AND card=? AND state=? "
                       "ORDER BY invocation DESC LIMIT 1");
  st.bind(1, run_id).bind(2, key(card)).bind(3, static_cast<int>(CardState::failed));
  if (!st.step()) return std::nullopt;
  return col_failure(st, 0);
}

std::vector<StoredValueMeta> History::value_metas(sql::Db& db, const std::string& run_id,
                                                  const CardPath& card, uint64_t n, bool input) {
  auto st = db.prepare(
      "SELECT v.port, v.kind, s.bfbs, v.root_type, v.size, v.artifact_id FROM ivalues v "
      "JOIN schemas s ON s.hash=v.schema_hash WHERE run_id=? AND card=? AND invocation=? AND side=? "
      "ORDER BY v.port");
  st.bind(1, run_id).bind(2, key(card)).bind(3, n).bind(4, input ? 0 : 1);
  std::vector<StoredValueMeta> out;
  while (st.step()) {
    StoredValueMeta m;
    m.name = st.text(0);
    m.kind = static_cast<PortKind>(st.i64(1));
    m.bfbs = st.blob(2);
    m.root_type = st.text(3);
    m.size_bytes = st.u64(4);
    if (!st.is_null(5)) m.artifact_id = st.text(5);
    out.push_back(std::move(m));
  }
  return out;
}

std::optional<std::string> History::value_bytes(sql::Db& db, const std::string& run_id,
                                                const CardPath& card, uint64_t n, bool input,
                                                const std::string& port) {
  auto st = db.prepare("SELECT bytes FROM ivalues WHERE run_id=? AND card=? AND invocation=? AND side=? AND port=?");
  st.bind(1, run_id).bind(2, key(card)).bind(3, n).bind(4, input ? 0 : 1).bind(5, port);
  if (!st.step()) return std::nullopt;
  return st.blob(0);
}

void History::put_last_port_input(sql::Db& db, const CardPath& card, const std::string& run_id, uint64_t invocation,
                                  const Value& value) {
  // Stored beside the per-Card rows under "<card>#<port>" ('#' never appears in ids).
  CardPath k = card;
  k.back() += "#" + value.name;
  put_last_input(db, k, run_id, invocation, {value});
}

std::optional<History::LastInput> History::last_port_input(sql::Db& db, const CardPath& card, const std::string& port) {
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
}

void History::overlay(InvocationSummaryData& s) {
  if (s.state != CardState::running && s.state != CardState::preparing) return;
  std::lock_guard<std::mutex> l(live_mu_);
  if (auto it = live_.find(live_key(s)); it != live_.end()) s = it->second;
}

void History::update_detail(sql::Db& db, const InvocationSummaryData& s) {
  db.prepare("UPDATE invocations SET detail=? WHERE run_id=? AND card=? AND invocation=?")
      .bind(1, detail_json(s)).bind(2, s.run_id).bind(3, key(s.card)).bind(4, s.invocation).run();
}

std::optional<History::LastInput> History::last_input(sql::Db& db, const CardPath& card) {
  auto st = db.prepare("SELECT run_id, invocation, inputs FROM last_inputs WHERE card=?");
  st.bind(1, key(card));
  if (!st.step()) return std::nullopt;
  LastInput li;
  li.run_id = st.text(0);
  li.invocation = st.u64(1);
  schema::SavedInputList list;
  if (schema::decode(st.text(2), list)) return std::nullopt;
  for (const auto& o : *list.items) {
    Value v;
    v.name = *o.name;
    v.kind = static_cast<PortKind>(*o.kind);
    auto t = std::make_shared<types::Type>();
    auto sst = db.prepare("SELECT bfbs FROM schemas WHERE hash=?");
    sst.bind(1, *o.schema);
    if (!sst.step()) return std::nullopt;
    t->bfbs = sst.blob(0);
    t->hash = *o.schema;
    t->root_type = *o.root_type;
    v.type = t;
    crypto::base64_decode(*o.data_b64, v.bytes);
    li.inputs.push_back(std::move(v));
  }
  return li;
}

namespace {
ArtifactData artifact_row(const sql::Stmt& st) {
  ArtifactData a;
  a.artifact_id = st.text(0);
  a.run_id = st.text(1);
  a.card = unkey(st.text(2));
  a.invocation = st.u64(3);
  a.output = st.text(4);
  a.kind = static_cast<PortKind>(st.i64(5));
  a.name = st.text(6);
  schema::MemberList list;
  if (!schema::decode(st.text(7), list))
    for (const auto& m : *list.items) a.members.push_back({*m.path, *m.size, *m.hash});
  return a;
}
}  // namespace

std::optional<ArtifactData> History::artifact(sql::Db& db, const std::string& artifact_id) {
  auto st = db.prepare("SELECT artifact_id, run_id, card, invocation, output, kind, name, members "
                       "FROM artifacts WHERE artifact_id=?");
  st.bind(1, artifact_id);
  if (!st.step()) return std::nullopt;
  return artifact_row(st);
}

std::vector<ArtifactData> History::artifacts(sql::Db& db, const std::string& run_id,
                                             const std::optional<CardPath>& card,
                                             std::optional<uint64_t> invocation,
                                             const std::string& after, uint64_t limit) {
  std::string q =
      "SELECT artifact_id, run_id, card, invocation, output, kind, name, members FROM artifacts "
      "WHERE run_id=?1 AND (?2 IS NULL OR card=?2) AND (?3 IS NULL OR invocation=?3) AND "
      "(card || '\x1f' || printf('%020d', invocation) || '\x1f' || output || '\x1f' || artifact_id) > ?4 "
      "ORDER BY card, invocation, output, artifact_id LIMIT ?5";
  auto st = db.prepare(q);
  st.bind(1, run_id);
  if (card) st.bind(2, key(*card));
  else st.bind_null(2);
  if (invocation) st.bind(3, *invocation);
  else st.bind_null(3);
  st.bind(4, after).bind(5, limit);
  std::vector<ArtifactData> out;
  while (st.step()) out.push_back(artifact_row(st));
  return out;
}

std::optional<DisplayData> History::display(sql::Db& db, const std::string& run_id, const CardPath& card) {
  auto st = db.prepare("SELECT data FROM displays WHERE run_id=? AND card=?");
  st.bind(1, run_id).bind(2, key(card));
  if (!st.step()) return std::nullopt;
  return wire::standalone_display(st.blob(0));
}

std::vector<RunSummaryData> History::runs_page(sql::Db& db, std::optional<CardPath> card,
                                               bool artifacts_only, int64_t below_seq,
                                               int64_t max_seq, uint64_t limit,
                                               std::vector<int64_t>* seqs) {
  std::string q = std::string("SELECT ") + kRunCols +
                  ", seq FROM runs WHERE seq<=?1 AND seq<?2 AND (?3=0 OR has_artifacts=1) AND "
                  "(?4 IS NULL OR target=?4 OR EXISTS(SELECT 1 FROM run_cards rc WHERE rc.run_id=runs.run_id AND rc.card=?4)) "
                  "ORDER BY seq DESC LIMIT ?5";
  auto st = db.prepare(q);
  st.bind(1, max_seq).bind(2, below_seq).bind(3, artifacts_only);
  if (card) st.bind(4, key(*card));
  else st.bind_null(4);
  st.bind(5, limit);
  std::vector<RunSummaryData> out;
  while (st.step()) {
    out.push_back(run_row(st));
    if (seqs) seqs->push_back(st.i64(11));
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
  st.bind(1, static_cast<int>(RunState::queued)).bind(2, static_cast<int>(RunState::preparing))
      .bind(3, static_cast<int>(RunState::running)).bind(4, static_cast<int>(RunState::cancelling));
  std::vector<std::string> out;
  while (st.step()) out.push_back(st.text(0));
  return out;
}

LogRead History::read_log(sql::Db& db, const std::string& run_id, const std::optional<CardPath>& card,
                          std::optional<uint64_t> invocation, LogStream stream, uint64_t offset,
                          bool tail, uint64_t limit) {
  int s = static_cast<int>(stream);
  const char* col = !card ? "run_off" : (invocation ? "inv_off" : "card_off");
  std::string where = "run_id=?1 AND stream=?2";
  if (card) where += " AND card=?3";
  if (card && invocation) where += " AND invocation=?4";
  auto bind = [&](sql::Stmt& st) {
    st.bind(1, run_id).bind(2, s);
    if (card) st.bind(3, key(*card));
    if (card && invocation) st.bind(4, *invocation);
  };
  LogRead out;
  {
    auto st = db.prepare(std::string("SELECT MAX(") + col + " + length(data)), COUNT(*) FROM logs WHERE " + where);
    bind(st);
    st.step();
    out.exists = st.i64(1) > 0;
    out.size = st.is_null(0) ? 0 : st.u64(0);
  }
  if (tail) offset = out.size > limit ? out.size - limit : 0;
  else if (offset > out.size) throw Failed(FailureKind::invalid_range, "offset is past the end of the log");
  uint64_t end = std::min(out.size, offset + limit + 4);  // a few extra bytes to find boundaries
  std::string buf;
  uint64_t buf_start = offset;
  {
    auto st = db.prepare(std::string("SELECT ") + col + ", data FROM logs WHERE " + where + " AND " + col +
                         " + length(data) > ?5 AND " + col + " < ?6 ORDER BY " + col);
    bind(st);
    st.bind(5, offset).bind(6, end);
    bool first = true;
    while (st.step()) {
      uint64_t o = st.u64(0);
      std::string d = st.blob(1);
      if (first) {
        buf_start = o;
        first = false;
      }
      buf += d;
    }
  }
  auto at = [&](uint64_t pos) -> int {
    if (pos < buf_start || pos - buf_start >= buf.size()) return -1;
    return static_cast<unsigned char>(buf[pos - buf_start]);
  };
  if (tail) {
    while (offset < out.size && at(offset) >= 0 && !char_start(static_cast<unsigned char>(at(offset)))) ++offset;
  } else if (offset < out.size && at(offset) >= 0 && !char_start(static_cast<unsigned char>(at(offset)))) {
    throw Failed(FailureKind::invalid_range, "offset is not on a UTF-8 character boundary");
  }
  uint64_t stop = std::min(out.size, offset + limit);
  while (stop > offset && stop < out.size && at(stop) >= 0 && !char_start(static_cast<unsigned char>(at(stop)))) --stop;
  if (offset >= buf_start && stop >= offset && buf.size() >= stop - buf_start)
    out.text = buf.substr(offset - buf_start, stop - offset);
  out.offset = offset;
  out.next_offset = offset + out.text.size();
  out.truncated = (tail && offset > 0) || out.next_offset < out.size;
  return out;
}

std::string History::make_cursor(const std::string& query_digest, int64_t hi, const std::string& after) const {
  schema::CursorJson c;
  *c.digest = query_digest;
  *c.hi = hi;
  *c.after = after;
  *c.expires = now_ms() + 30 * 60 * 1000;
  std::string payload = crypto::base64(schema::encode(c));
  std::string sig = crypto::sha256_hex(secret_ + payload).substr(0, 32);
  return payload + "." + sig;
}

std::pair<int64_t, std::string> History::open_cursor(const std::string& cursor,
                                                     const std::string& query_digest) const {
  auto dot = cursor.rfind('.');
  if (dot == std::string::npos) throw Failed(FailureKind::invalid_cursor, "malformed cursor");
  std::string payload = cursor.substr(0, dot);
  if (!crypto::equal_secret(crypto::sha256_hex(secret_ + payload).substr(0, 32), cursor.substr(dot + 1)))
    throw Failed(FailureKind::invalid_cursor, "cursor signature mismatch");
  std::string raw;
  schema::CursorJson c;
  if (!crypto::base64_decode(payload, raw) || schema::decode(raw, c))
    throw Failed(FailureKind::invalid_cursor, "malformed cursor");
  if (*c.digest != query_digest) throw Failed(FailureKind::invalid_cursor, "cursor belongs to a different query");
  if (*c.expires < now_ms()) throw Failed(FailureKind::invalid_cursor, "cursor expired");
  return {*c.hi, *c.after};
}

std::string History::schema_blob(sql::Db& db, const std::string& hash) {
  auto st = db.prepare("SELECT bfbs FROM schemas WHERE hash=?");
  st.bind(1, hash);
  if (!st.step()) return {};
  return st.blob(0);
}

}  // namespace tsugumi
