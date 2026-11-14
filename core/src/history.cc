#include "history.hh"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <functional>
#include <system_error>

#include "util/crypto.hh"
#include "util/fs.hh"
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
  auto& stages = d.stages->emplace();
  for (const auto& st : s.stages) {
    schema::StageJson j;
    *j.phase = static_cast<int>(st.phase);
    *j.outcome = static_cast<int>(st.outcome);
    *j.start = st.start;
    *j.end = st.end.value_or(-1);
    stages.push_back(std::move(j));
  }
  *d.worker_reused = s.worker_reused ? (*s.worker_reused ? 1 : 0) : -1;
  *d.outputs_sent = s.outputs_sent;
  *d.ready_at = s.ready_at.value_or(-1);
  *d.emulator = s.emulator;
  *d.environment_id = s.environment_id;
  *d.params = s.params_json;
  *d.execution_service = s.execution_service;
  *d.execution_job_id = s.execution_job_id;
  *d.execution_system = s.execution_system;
  *d.execution_cpu_millis = s.execution_resources ? s.execution_resources->cpu_millis : 0;
  *d.execution_memory_bytes = s.execution_resources ? s.execution_resources->memory_bytes : 0;
  *d.execution_state = s.execution_state ? static_cast<int64_t>(*s.execution_state) : -1;
  *d.batch_activation = s.batch_activation.value_or(0);
  *d.batch_index = s.batch_index.value_or(0);
  *d.batch_size = s.batch_size.value_or(0);
  *d.implementation_index = s.implementation_index.value_or(0);
  return schema::encode(d);
}

void detail_into(const std::string& text, InvocationSummaryData& s) {
  schema::InvocationDetailJson d;
  if (text.empty() || schema::decode(text, d)) return;
  s.stages.clear();
  if (const auto* stages = schema::get(d.stages))
    for (const auto& j : *stages) {
      StageTimeData st;
      st.phase = static_cast<Phase>(*j.phase);
      st.outcome = static_cast<StageOutcome>(*j.outcome);
      st.start = *j.start;
      if (*j.end >= 0) st.end = *j.end;
      s.stages.push_back(st);
    }
  if (auto reused = d.worker_reused->value_or(-1); reused >= 0) s.worker_reused = reused == 1;
  if (const auto* outputs = schema::get(d.outputs_sent)) s.outputs_sent = *outputs;
  if (auto ready = d.ready_at->value_or(-1); ready >= 0) s.ready_at = ready;
  s.emulator = d.emulator->value_or("");
  s.environment_id = d.environment_id->value_or("");
  s.params_json = d.params->value_or("");
  if (const auto* service = schema::get(d.execution_service)) s.execution_service = *service;
  s.execution_job_id = d.execution_job_id->value_or("");
  s.execution_system = d.execution_system->value_or("");
  const auto cpu = d.execution_cpu_millis->value_or(0);
  const auto memory = d.execution_memory_bytes->value_or(0);
  if (cpu > 0 && cpu <= UINT32_MAX && memory > 0)
    s.execution_resources = ExecutionResources{static_cast<uint32_t>(cpu), memory};
  const auto state = d.execution_state->value_or(-1);
  if (state >= 0 && state <= static_cast<int64_t>(RemoteExecutionState::interrupted))
    s.execution_state = static_cast<RemoteExecutionState>(state);
  if (const auto activation = d.batch_activation->value_or(0); activation > 0) s.batch_activation = activation;
  if (const auto index = d.batch_index->value_or(0); index > 0 && index <= UINT32_MAX)
    s.batch_index = static_cast<uint32_t>(index);
  if (const auto size = d.batch_size->value_or(0); size > 0 && size <= UINT32_MAX)
    s.batch_size = static_cast<uint32_t>(size);
  if (const auto implementation = d.implementation_index->value_or(0);
      implementation > 0 && implementation <= UINT32_MAX)
    s.implementation_index = static_cast<uint32_t>(implementation);
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
  if (!st.is_null(11)) s.status.commit = st.text(11);
  return s;
}

constexpr const char* kRunCols =
    "run_id, state, rev, execution_rev, accepted_at, started_at, ended_at, failure, source_hash, "
    "target, propagate, (SELECT commit_id FROM run_commits c WHERE c.run_id = runs.run_id)";

InvocationSummaryData inv_row(const sql::Stmt& st, const std::string& run_id, int cached_col = -1,
                              int detail_col = -1, int service_col = -1) {
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
  if (service_col >= 0 && !st.is_null(service_col)) s.execution_service = st.text(service_col);
  return s;
}

bool char_start(unsigned char c) { return (c & 0xC0) != 0x80; }

// The tables a run lives in, the same in the history and in its copy
// (.tsugumi/runs.sqlite); `p` is the schema prefix ("" or "ex.").
std::string run_tables(const std::string& p) {
  return "CREATE TABLE IF NOT EXISTS " + p + R"SQL(runs(
  seq INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id TEXT NOT NULL UNIQUE, op_id TEXT NOT NULL,
  state INTEGER NOT NULL, rev INTEGER, execution_rev INTEGER,
  accepted_at INTEGER, started_at INTEGER, ended_at INTEGER, failure BLOB,
  source_hash TEXT NOT NULL, target TEXT NOT NULL, propagate INTEGER NOT NULL,
  has_artifacts INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(build_environments(
  run_id TEXT NOT NULL, id TEXT NOT NULL, derivation TEXT NOT NULL, manifest TEXT NOT NULL,
  system TEXT NOT NULL, path_count INTEGER NOT NULL, PRIMARY KEY(run_id, id));
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(environment_paths(
  run_id TEXT NOT NULL, environment_id TEXT NOT NULL, position INTEGER NOT NULL, path TEXT NOT NULL,
  PRIMARY KEY(run_id, environment_id, position));
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(run_substituters(
  run_id TEXT NOT NULL, position INTEGER NOT NULL, url TEXT NOT NULL, PRIMARY KEY(run_id, position));
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(run_cards(run_id TEXT NOT NULL, card TEXT NOT NULL, PRIMARY KEY(run_id, card));
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(run_batches(
  run_id TEXT NOT NULL, card TEXT NOT NULL, activation INTEGER NOT NULL, invocation_count INTEGER NOT NULL,
  execution_list INTEGER NOT NULL, complete INTEGER NOT NULL, failed INTEGER NOT NULL, outputs TEXT NOT NULL,
  PRIMARY KEY(run_id, card, activation));
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(run_commits(run_id TEXT PRIMARY KEY, commit_id TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(invocations(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL,
  state INTEGER NOT NULL, phase INTEGER, started_at INTEGER, ended_at INTEGER,
  duration_ms INTEGER, failure BLOB, waiting TEXT, record_io INTEGER NOT NULL,
  input_sources TEXT, display BLOB, build_cached INTEGER, detail TEXT,
  PRIMARY KEY(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(schemas(hash TEXT PRIMARY KEY, root_type TEXT NOT NULL, bfbs BLOB NOT NULL);
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(ivalues(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL, side INTEGER NOT NULL,
  port TEXT NOT NULL, kind INTEGER NOT NULL, schema_hash TEXT NOT NULL, root_type TEXT NOT NULL,
  size INTEGER NOT NULL, bytes BLOB, hash TEXT NOT NULL, artifact_id TEXT,
  PRIMARY KEY(run_id, card, invocation, side, port));
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(logs(
  id INTEGER PRIMARY KEY AUTOINCREMENT, run_id TEXT NOT NULL, card TEXT NOT NULL,
  invocation INTEGER NOT NULL, stream INTEGER NOT NULL,
  run_off INTEGER NOT NULL, card_off INTEGER NOT NULL, inv_off INTEGER NOT NULL, data BLOB NOT NULL);
CREATE INDEX IF NOT EXISTS )SQL" + p + R"SQL(logs_run ON logs(run_id, stream, run_off);
CREATE INDEX IF NOT EXISTS )SQL" + p + R"SQL(logs_inv ON logs(run_id, card, invocation, stream, inv_off);
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(artifacts(
  artifact_id TEXT PRIMARY KEY, run_id TEXT NOT NULL, card TEXT NOT NULL,
  invocation INTEGER NOT NULL, output TEXT NOT NULL, kind INTEGER NOT NULL, name TEXT NOT NULL,
  members TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS )SQL" + p + R"SQL(artifacts_run ON artifacts(run_id, card, invocation);
CREATE TABLE IF NOT EXISTS )SQL" + p + R"SQL(displays(
  run_id TEXT NOT NULL, card TEXT NOT NULL, revision INTEGER NOT NULL, data BLOB NOT NULL,
  PRIMARY KEY(run_id, card));
)SQL";
}

// Version of the local history's tables (PRAGMA user_version).
constexpr int kHistoryVersion = 5;
constexpr const char* kCopyFormat = "tsugumi-runs";
constexpr const char* kCopyVersion = "4";

namespace stdfs = std::filesystem;

constexpr const char* kHistorySuffixes[] = {"", "-wal", "-shm", "-journal"};

struct HistoryArchive {
  stdfs::path database;
  stdfs::path directory;
  std::string id;
  bool pending;
};

auto history_file(const stdfs::path& path, const char* suffix) -> stdfs::path {
  return path.string() + suffix;
}

auto regular_history_file(const stdfs::path& path) -> bool {
  const auto status = stdfs::symlink_status(path);
  if (!stdfs::exists(status)) return false;
  if (!stdfs::is_regular_file(status)) throw std::runtime_error("history is not a regular file: " + path.string());
  return true;
}

auto archive_intent(const stdfs::path& database) -> stdfs::path {
  return history_file(database, ".archive-pending");
}

auto read_archive_id(const stdfs::path& intent) -> std::string {
  // Read one UUID, with space for an extra byte to reject longer contents.
  char buffer[37];
  const int fd = ::open(intent.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "open " + intent.string());
  ssize_t count;
  do { count = ::read(fd, buffer, sizeof buffer); } while (count < 0 && errno == EINTR);
  const int error = errno;
  ::close(fd);
  if (count < 0) throw std::system_error(error, std::generic_category(), "read " + intent.string());
  const std::string id(buffer, static_cast<size_t>(count));
  if (!crypto::is_uuid(id)) throw std::runtime_error("invalid history archive intent: " + intent.string());
  return id;
}

auto sync_history_directory(const stdfs::path& directory) -> void {
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "open " + directory.string());
  int result;
  do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
  const int error = errno;
  ::close(fd);
  if (result < 0) throw std::system_error(error, std::generic_category(), "sync " + directory.string());
}

auto history_archive(const stdfs::path& database, std::string id, bool pending) -> HistoryArchive {
  const auto directory = database.parent_path() / ("history-archive-" + id);
  const auto status = stdfs::symlink_status(directory);
  if (stdfs::exists(status) && (!pending || !stdfs::is_directory(status)))
    throw std::runtime_error("invalid history archive directory: " + directory.string());
  for (const auto* suffix : kHistorySuffixes) {
    const auto source = history_file(database, suffix);
    const auto destination = directory / source.filename();
    const bool has_source = regular_history_file(source);
    const bool has_destination = regular_history_file(destination);
    if (has_source && has_destination)
      throw std::runtime_error("history archive has both source and destination: " + source.string());
    if (!*suffix && !has_source && !has_destination)
      throw std::runtime_error("history archive is missing its database: " + database.string());
  }
  return {database, directory, std::move(id), pending};
}

auto finish_history_archive(const HistoryArchive& archive, std::string_view fault) -> void {
  const auto intent = archive_intent(archive.database);
  if (!archive.pending) {
    fs::write_atomic(intent, archive.id, 0600);
    sync_history_directory(intent.parent_path());
  }
  // The project lock excludes other Tsugumi processes. Leave the intent on
  // any failure so no startup can create a fresh database over its old WAL.
  stdfs::create_directory(archive.directory);
  stdfs::permissions(archive.directory, stdfs::perms::owner_all);
  for (const auto* suffix : kHistorySuffixes) {
    const auto source = history_file(archive.database, suffix);
    if (!regular_history_file(source)) continue;
    const auto destination = archive.directory / source.filename();
    if (regular_history_file(destination))
      throw std::runtime_error("history archive destination already exists: " + destination.string());
    stdfs::rename(source, destination);
    if (!*suffix && fault == "history_archive_after_main") ::_exit(97);
  }
  sync_history_directory(archive.directory);
  sync_history_directory(intent.parent_path());
  fs::remove_file_durable(intent);
  sync_history_directory(intent.parent_path());
  log::info("incompatible history archived in " + archive.directory.string());
}

}  // namespace

History::History(const std::filesystem::path& db_path, std::string cursor_secret, std::string fault)
    : path_(db_path), secret_(std::move(cursor_secret)), fault_(std::move(fault)) {
  if (std::filesystem::exists(path_)) check_version();
  writer_db_ = std::make_unique<sql::Db>(path_.string());
  // Single process owns the database (guarded by the project lock).
  writer_db_->exec("PRAGMA journal_mode=WAL");
  writer_db_->exec("PRAGMA synchronous=FULL");
  writer_db_->exec("PRAGMA foreign_keys=ON");
  init_schema(*writer_db_);
  writer_ = std::thread([this] { writer_loop(); });
}

// A fresh database has no tables or version. A history written with a
// different schema is rejected before the writer opens it.
auto History::check_version() const -> void {
  sql::Db db(path_.string(), true);
  auto version = db.prepare("PRAGMA user_version");
  const auto stored = version.step() ? version.i64(0) : 0;
  if (stored == kHistoryVersion) return;
  auto tables = db.prepare("SELECT 1 FROM sqlite_master WHERE type='table' LIMIT 1");
  if (stored == 0 && !tables.step()) return;
  throw IncompatibleHistory("the run history " + path_.string() + " has schema version " +
                           std::to_string(stored) + ", but this Tsugumi requires " +
                           std::to_string(kHistoryVersion) + ". The history file is kept as it is.",
                           std::to_string(stored), std::to_string(kHistoryVersion), "local");
}

auto History::prepare_storage(const stdfs::path& local, const stdfs::path& copy,
                              bool archive_incompatible, std::string_view fault) -> void {
  std::vector<HistoryArchive> archives;
  std::optional<IncompatibleHistory> first;
  // Validate both databases before moving anything. Corruption is not a version mismatch.
  for (const auto& path : {local, copy}) {
    const bool is_local = path == local;
    const std::string expected = is_local ? std::to_string(kHistoryVersion) : kCopyVersion;
    const char* source = is_local ? "local" : "portable";
    const auto intent = archive_intent(path);
    if (regular_history_file(intent)) {
      archives.push_back(history_archive(path, read_archive_id(intent), true));
      if (!first) first.emplace("the run history " + path.string() +
          " has an unfinished archive. Resume the archive to open the project.", "pending", expected, source);
      continue;
    }
    const bool present = regular_history_file(path);
    for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
      const auto sidecar = history_file(path, suffix);
      if (regular_history_file(sidecar) && !present)
        throw std::runtime_error("history sidecar has no database: " + sidecar.string());
    }
    if (!present) continue;

    sql::Db db(path.string(), true);
    if (archive_incompatible) {
      auto check = db.prepare("PRAGMA quick_check");
      if (!check.step() || check.text(0) != "ok")
        throw std::runtime_error("history integrity check failed: " + path.string());
    }
    std::string found;
    if (is_local) {
      auto version = db.prepare("PRAGMA user_version");
      const auto stored = version.step() ? version.i64(0) : 0;
      auto tables = db.prepare("SELECT 1 FROM sqlite_master WHERE type='table' LIMIT 1");
      if (stored == kHistoryVersion || (stored == 0 && !tables.step())) continue;
      found = std::to_string(stored);
    } else {
      auto version = db.prepare("SELECT key, value FROM meta WHERE key IN ('format','version')");
      std::map<std::string, std::string> meta;
      while (version.step()) meta[version.text(0)] = version.text(1);
      if (meta["format"] != kCopyFormat || meta["version"].empty())
        throw std::runtime_error("unrecognized history copy: " + path.string());
      if (meta["version"] == kCopyVersion) continue;
      found = meta["version"];
    }
    archives.push_back(history_archive(path, crypto::uuid4(), false));
    if (!first) first.emplace("the run history " + path.string() + " has schema version " + found +
        ", but this Tsugumi requires " + expected + ". The history file is kept as it is.",
        found, expected, source);
  }
  if (!first) return;
  if (!archive_incompatible) throw *first;
  for (const auto& archive : archives) finish_history_archive(archive, fault);
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
CREATE TABLE IF NOT EXISTS active_environments(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL, environment_id TEXT NOT NULL,
  PRIMARY KEY(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS statuses(
  run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL,
  state INTEGER NOT NULL, phase INTEGER, started_at INTEGER, failure BLOB, waiting TEXT,
  updated_at INTEGER NOT NULL, execution_service TEXT NOT NULL, detail TEXT NOT NULL,
  PRIMARY KEY(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS remote_jobs(
  job_id TEXT PRIMARY KEY, run_id TEXT NOT NULL, card TEXT NOT NULL, invocation INTEGER NOT NULL,
  instance TEXT NOT NULL, server_id TEXT NOT NULL, spec TEXT NOT NULL, contract TEXT NOT NULL, directory TEXT NOT NULL,
  accepted INTEGER NOT NULL, cancel_requested INTEGER NOT NULL, detail TEXT NOT NULL, batch_activation INTEGER NOT NULL,
  UNIQUE(run_id, card, invocation));
CREATE TABLE IF NOT EXISTS remote_releases(
  job_id TEXT PRIMARY KEY, service TEXT NOT NULL, server_id TEXT NOT NULL,
  instance TEXT NOT NULL, fingerprint TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS last_inputs(
  card TEXT PRIMARY KEY, run_id TEXT NOT NULL, invocation INTEGER NOT NULL, inputs TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS receipts(
  op_id TEXT PRIMARY KEY, kind TEXT NOT NULL, digest TEXT NOT NULL, result BLOB NOT NULL);
)SQL");
  db.exec("PRAGMA user_version=" + std::to_string(kHistoryVersion));
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

auto History::put_environment(sql::Db& db, const std::string& run_id, const BuildEnvironmentData& e) -> void {
  db.prepare("INSERT OR IGNORE INTO build_environments VALUES(?,?,?,?,?,?)")
      .bind(1, run_id)
      .bind(2, e.id)
      .bind(3, e.derivation)
      .bind(4, e.manifest)
      .bind(5, e.system)
      .bind(6, static_cast<uint64_t>(e.paths.size()))
      .run();
  if (!db.changes()) return;
  auto insert = db.prepare("INSERT INTO environment_paths VALUES(?,?,?,?)");
  for (size_t i = 0; i < e.paths.size(); ++i) {
    insert.bind(1, run_id).bind(2, e.id).bind(3, static_cast<uint64_t>(i)).bind(4, e.paths[i]).run();
    insert.reset();
  }
}

auto History::put_substituters(sql::Db& db, const std::string& run_id, const std::vector<std::string>& urls) -> void {
  auto insert = db.prepare("INSERT OR IGNORE INTO run_substituters VALUES(?,?,?)");
  for (size_t i = 0; i < urls.size(); ++i) {
    insert.bind(1, run_id).bind(2, static_cast<uint64_t>(i)).bind(3, urls[i]).run();
    insert.reset();
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
  if (s.status.commit)
    db.prepare("INSERT OR REPLACE INTO run_commits(run_id, commit_id) VALUES(?,?)").bind(1, *s.status.run_id).bind(2, *s.status.commit).run();
  if (s.status.failure) update_run(db, s.status);
}

void History::update_run(sql::Db& db, const RunStatusData& s) {
  auto st = db.prepare("UPDATE runs SET state=?, started_at=?, ended_at=?, failure=? WHERE run_id=?");
  st.bind(1, static_cast<int>(s.state)).bind(2, s.started_at).bind(3, s.ended_at);
  bind_failure(st, 4, s.failure);
  st.bind(5, *s.run_id).run();
}

void History::put_status(sql::Db& db, const InvocationSummaryData& s) {
  if (!s.environment_id.empty())
    db.prepare("INSERT OR REPLACE INTO active_environments VALUES(?,?,?,?)")
        .bind(1, s.run_id).bind(2, key(s.card)).bind(3, s.invocation).bind(4, s.environment_id).run();
  auto st = db.prepare(
      "INSERT INTO statuses(run_id, card, invocation, state, phase, started_at, failure, waiting, updated_at, execution_service, detail) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(run_id, card, invocation) DO UPDATE SET "
      "state=excluded.state, phase=excluded.phase, started_at=excluded.started_at, "
      "failure=excluded.failure, waiting=excluded.waiting, updated_at=excluded.updated_at, "
      "execution_service=excluded.execution_service, detail=excluded.detail");
  st.bind(1, s.run_id).bind(2, key(s.card)).bind(3, s.invocation).bind(4, static_cast<int>(s.state));
  if (s.phase) st.bind(5, static_cast<int>(*s.phase));
  else st.bind_null(5);
  st.bind(6, s.started_at);
  bind_failure(st, 7, s.failure);
  st.bind(8, waiting_json(s.waiting)).bind(9, now_ms()).bind(10, s.execution_service).bind(11, detail_json(s)).run();
  db.prepare("INSERT OR IGNORE INTO run_cards(run_id, card) VALUES(?,?)")
      .bind(1, s.run_id).bind(2, key(s.card)).run();
}

auto History::put_batch(sql::Db& db, const RunBatchData& batch) -> void {
  schema::BatchOutputList outputs;
  for (const auto& output : batch.outputs) {
    schema::BatchOutputJson entry;
    *entry.port = output.port;
    *entry.count = output.count;
    *entry.execution_list = output.execution_list;
    outputs.outputs->push_back(std::move(entry));
  }
  const auto card = key(batch.card);
  db.prepare("INSERT OR REPLACE INTO run_batches VALUES(?,?,?,?,?,?,?,?)")
      .bind(1, batch.run_id).bind(2, card).bind(3, batch.activation)
      .bind(4, static_cast<uint64_t>(batch.invocation_count)).bind(5, batch.execution_list)
      .bind(6, batch.complete).bind(7, batch.failed).bind(8, schema::encode(outputs)).run();
  db.prepare("INSERT OR IGNORE INTO run_cards(run_id, card) VALUES(?,?)").bind(1, batch.run_id).bind(2, card).run();
}

auto History::interrupt_batches(sql::Db& db, const std::string& run_id) -> void {
  db.prepare("UPDATE run_batches SET complete=1, failed=1, outputs=? WHERE run_id=? AND complete=0")
      .bind(1, schema::encode(schema::BatchOutputList{})).bind(2, run_id).run();
}

auto History::put_remote_job(sql::Db& db, const RemoteJobRecord& job) -> void {
  const auto& inv = job.invocation;
  auto st = db.prepare("INSERT INTO remote_jobs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(job_id) DO UPDATE SET "
                       "accepted=max(accepted,excluded.accepted), cancel_requested=max(cancel_requested,excluded.cancel_requested), "
                       "detail=excluded.detail");
  st.bind(1, inv.execution_job_id).bind(2, inv.run_id).bind(3, key(inv.card)).bind(4, inv.invocation)
      .bind(5, job.instance).bind(6, job.server_id).bind(7, job.spec).bind(8, job.contract).bind(9, job.directory)
      .bind(10, job.accepted).bind(11, job.cancel_requested).bind(12, detail_json(inv))
      .bind(13, inv.batch_activation.value_or(0)).run();
}

auto History::cancel_remote_jobs(sql::Db& db, const std::string& run_id) -> void {
  db.prepare("UPDATE remote_jobs SET cancel_requested=1 WHERE run_id=?").bind(1, run_id).run();
}

auto History::cancel_remote_batch(sql::Db& db, const std::string& run_id, const CardPath& card,
                                  uint64_t activation) -> void {
  db.prepare("UPDATE remote_jobs SET cancel_requested=1 WHERE run_id=? AND card=? AND batch_activation=?")
      .bind(1, run_id).bind(2, key(card)).bind(3, activation).run();
}

auto History::has_remote_jobs(sql::Db& db) -> bool {
  auto st = db.prepare("SELECT 1 FROM remote_jobs LIMIT 1");
  return st.step();
}

auto History::remote_job_card(sql::Db& db, const std::string& job_id) -> std::optional<CardPath> {
  auto st = db.prepare("SELECT card FROM remote_jobs WHERE job_id=?");
  st.bind(1, job_id);
  if (!st.step()) return std::nullopt;
  return path_from_key(st.text(0));
}

auto History::remote_jobs(sql::Db& db) -> std::vector<RemoteJobRecord> {
  auto st = db.prepare("SELECT run_id,card,invocation,instance,server_id,spec,contract,directory,accepted,cancel_requested,detail,job_id "
                       "FROM remote_jobs ORDER BY run_id,card,invocation");
  std::vector<RemoteJobRecord> jobs;
  while (st.step()) {
    const auto id = st.text(0);
    if (!crypto::is_uuid(id) || !crypto::is_sha256_hex(st.text(3)) || !crypto::is_sha256_hex(st.text(4)) ||
        !crypto::is_sha256_hex(st.text(11)))
      throw std::runtime_error("invalid saved remote job identity");
    const auto card = path_from_key(st.text(1));
    auto inv = status(db, id, card, st.u64(2));
    if (!inv) throw std::runtime_error("remote job has no invocation status");
    detail_into(st.text(10), *inv);
    if (inv->execution_job_id != st.text(11)) throw std::runtime_error("remote job identity differs from its invocation");
    jobs.push_back({std::move(*inv), st.text(3), st.text(4), st.text(5), st.text(6), st.text(7),
                    st.i64(8) != 0, st.i64(9) != 0});
  }
  return jobs;
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
  db.prepare("DELETE FROM active_environments WHERE run_id=? AND card=? AND invocation=?")
      .bind(1, s.run_id).bind(2, key(s.card)).bind(3, s.invocation).run();
  db.prepare("DELETE FROM statuses WHERE run_id=? AND card=? AND invocation=?")
      .bind(1, s.run_id).bind(2, key(s.card)).bind(3, s.invocation).run();
  if (!s.execution_job_id.empty()) {
    auto pending = db.prepare("SELECT server_id,instance,spec FROM remote_jobs WHERE job_id=?");
    pending.bind(1, s.execution_job_id);
    if (pending.step())
      db.prepare("INSERT OR REPLACE INTO remote_releases VALUES(?,?,?,?,?)")
        .bind(1, s.execution_job_id).bind(2, s.execution_service).bind(3, pending.text(0))
        .bind(4, pending.text(1)).bind(5, crypto::sha256_hex(pending.text(2))).run();
    db.prepare("DELETE FROM remote_jobs WHERE job_id=?").bind(1, s.execution_job_id).run();
  }
  db.prepare("INSERT OR IGNORE INTO run_cards(run_id, card) VALUES(?,?)")
      .bind(1, s.run_id).bind(2, key(s.card)).run();
  if (r.record_io) {
    auto put = [&](const Value& v, int side) {
      std::string h = put_schema(db, *v.type);
      auto ins = db.prepare(
          "INSERT OR REPLACE INTO ivalues(run_id, card, invocation, side, port, kind, schema_hash, "
          "root_type, size, bytes, hash, artifact_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
      ins.bind(1, s.run_id).bind(2, key(s.card)).bind(3, s.invocation).bind(4, side).bind(5, v.name)
          .bind(6, static_cast<int>(v.kind)).bind(7, h).bind(8, v.type->root_type)
          .bind(9, static_cast<uint64_t>(v.bytes.size())).bind_blob(10, v.bytes)
          .bind(11, crypto::sha256_hex(v.bytes)).bind(12, v.artifact_id)
          .run();
    };
    for (const auto& v : r.inputs) put(v, 0);
    for (const auto& v : r.outputs) put(v, 1);
  }
}

auto History::put_last_input(sql::Db& db, const CardPath& card, const std::string& run_id,
                             uint64_t activation, const std::vector<SavedInputGroup>& groups) -> void {
  schema::SavedInputList list;
  for (const auto& group : groups) {
    schema::SavedInputGroupJson entry;
    *entry.port = group.port;
    *entry.execution_list = group.execution_list;
    for (const auto& v : group.values) {
      schema::SavedInputJson j;
      *j.kind = static_cast<int>(v.kind);
      *j.schema = put_schema(db, *v.type);
      *j.root_type = v.type->root_type;
      *j.data_b64 = crypto::base64(v.bytes);
      entry.values->push_back(std::move(j));
    }
    list.groups->push_back(std::move(entry));
  }
  db.prepare("INSERT OR REPLACE INTO last_inputs(card, run_id, invocation, inputs) VALUES(?,?,?,?)")
      .bind(1, key(card)).bind(2, run_id).bind(3, activation).bind(4, schema::encode(list)).run();
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

std::vector<InvocationSummaryData> History::latest_invocations(sql::Db& db, const std::string& run_id) {
  auto st = db.prepare(
      "SELECT card, invocation, state, phase, started_at, ended_at, duration_ms, failure, waiting, build_cached, detail, NULL "
      "FROM invocations i WHERE run_id=?1 AND invocation=(SELECT MAX(invocation) FROM invocations WHERE run_id=?1 AND card=i.card) "
      "UNION ALL SELECT card, invocation, state, phase, started_at, NULL, NULL, failure, waiting, NULL, detail, execution_service "
      "FROM statuses s WHERE run_id=?1 AND invocation=(SELECT MAX(invocation) FROM statuses WHERE run_id=?1 AND card=s.card) "
      "AND invocation > COALESCE((SELECT MAX(invocation) FROM invocations WHERE run_id=?1 AND card=s.card), 0) "
      "ORDER BY card, invocation");
  st.bind(1, run_id);
  std::vector<InvocationSummaryData> out;
  while (st.step()) {
    auto row = inv_row(st, run_id, 9, 10, 11);
    // A status newer than the Card's last record is the one a client wants.
    if (!out.empty() && out.back().card == row.card) out.back() = std::move(row);
    else out.push_back(std::move(row));
  }
  return out;
}

namespace {

auto batch_row(const sql::Stmt& row, const std::string& run_id) -> RunBatchData {
  RunBatchData batch;
  batch.run_id = run_id;
  batch.card = unkey(row.text(0));
  batch.activation = row.u64(1);
  const auto count = row.u64(2);
  if (count > UINT32_MAX) throw std::runtime_error("invalid invocation count in run batch history");
  batch.invocation_count = static_cast<uint32_t>(count);
  batch.execution_list = row.i64(3) != 0;
  batch.complete = row.i64(4) != 0;
  batch.failed = row.i64(5) != 0;
  schema::BatchOutputList outputs;
  if (schema::decode(row.text(6), outputs)) throw std::runtime_error("invalid outputs in run batch history");
  for (const auto& entry : *outputs.outputs) {
    if (*entry.count > UINT32_MAX) throw std::runtime_error("invalid output count in run batch history");
    batch.outputs.push_back({*entry.port, static_cast<uint32_t>(*entry.count), *entry.execution_list});
  }
  return batch;
}

}  // namespace

auto History::latest_batches(sql::Db& db, const std::string& run_id, const std::optional<CardPath>& card)
    -> std::vector<RunBatchData> {
  auto st = db.prepare(
      "SELECT card, activation, invocation_count, execution_list, complete, failed, outputs FROM run_batches b "
      "WHERE run_id=?1 AND (?2 IS NULL OR card=?2) AND activation=(SELECT MAX(activation) FROM run_batches "
      "WHERE run_id=?1 AND card=b.card) ORDER BY card");
  st.bind(1, run_id);
  if (card) st.bind(2, key(*card));
  else st.bind_null(2);
  std::vector<RunBatchData> out;
  while (st.step()) out.push_back(batch_row(st, run_id));
  return out;
}

std::vector<InvocationSummaryData> History::invocations(sql::Db& db, const std::string& run_id,
                                                        const CardPath& card, uint64_t after,
                                                        uint64_t limit) {
  // Completed records and in-progress statuses, ascending invocation number.
  auto st = db.prepare(
      "SELECT card, invocation, state, phase, started_at, ended_at, duration_ms, failure, waiting, build_cached, detail, NULL "
      "FROM invocations WHERE run_id=?1 AND card=?2 AND invocation>?3 "
      "UNION ALL SELECT card, invocation, state, phase, started_at, NULL, NULL, failure, waiting, NULL, detail, execution_service "
      "FROM statuses WHERE run_id=?1 AND card=?2 AND invocation>?3 AND invocation NOT IN "
      "(SELECT invocation FROM invocations WHERE run_id=?1 AND card=?2) "
      "ORDER BY invocation LIMIT ?4");
  st.bind(1, run_id).bind(2, key(card)).bind(3, after).bind(4, limit);
  std::vector<InvocationSummaryData> out;
  while (st.step()) out.push_back(inv_row(st, run_id, 9, 10, 11));
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
      "SELECT card, invocation, state, phase, started_at, NULL, NULL, failure, waiting, detail, execution_service "
      "FROM statuses WHERE run_id=? AND card=? AND invocation=?");
  st.bind(1, run_id).bind(2, key(card)).bind(3, n);
  if (!st.step()) return std::nullopt;
  return inv_row(st, run_id, -1, 9, 10);
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
      "SELECT v.port, v.kind, s.bfbs, v.root_type, v.size, v.artifact_id, v.hash FROM ivalues v "
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
    if (!st.is_null(6)) m.hash = st.text(6);
    out.push_back(std::move(m));
  }
  return out;
}

std::optional<History::ValuePage> History::value_page(sql::Db& db, const std::string& run_id, const CardPath& card,
                                                      uint64_t n, bool input, const std::string& port, uint64_t offset,
                                                      uint64_t limit) {
  auto st = db.prepare("SELECT substr(bytes, ?, ?), size, hash FROM ivalues "
                       "WHERE run_id=? AND card=? AND invocation=? AND side=? AND port=?");
  st.bind(1, offset + 1).bind(2, limit).bind(3, run_id).bind(4, key(card)).bind(5, n).bind(6, input ? 0 : 1).bind(7, port);
  if (!st.step()) return std::nullopt;
  ValuePage p;
  if (!st.is_null(0)) p.bytes = st.blob(0);
  p.size = static_cast<uint64_t>(st.i64(1));
  p.hash = st.text(2);
  return p;
}

std::optional<std::string> History::value_bytes(sql::Db& db, const std::string& run_id,
                                                const CardPath& card, uint64_t n, bool input,
                                                const std::string& port) {
  auto st = db.prepare("SELECT bytes FROM ivalues WHERE run_id=? AND card=? AND invocation=? AND side=? AND port=?");
  st.bind(1, run_id).bind(2, key(card)).bind(3, n).bind(4, input ? 0 : 1).bind(5, port);
  if (!st.step()) return std::nullopt;
  return st.blob(0);
}

auto History::put_last_port_input(sql::Db& db, const CardPath& card, const std::string& run_id, uint64_t activation,
                                  const SavedInputGroup& group) -> void {
  // Stored beside the per-Card rows under "<card>#<port>" ('#' never appears in ids).
  CardPath k = card;
  k.back() += "#" + group.port;
  put_last_input(db, k, run_id, activation, {group});
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
  std::map<std::pair<std::string, std::string>, types::TypeRef> loaded_types;
  for (const auto& entry : *list.groups) {
    SavedInputGroup group{*entry.port, *entry.execution_list, {}};
    for (const auto& value : *entry.values) {
      Value v;
      v.name = group.port;
      v.kind = static_cast<PortKind>(*value.kind);
      auto [type, inserted] = loaded_types.try_emplace(std::make_pair(*value.schema, *value.root_type));
      if (inserted) {
        auto schema_row = db.prepare("SELECT bfbs FROM schemas WHERE hash=?");
        schema_row.bind(1, *value.schema);
        if (!schema_row.step()) return std::nullopt;
        auto decoded = std::make_shared<types::Type>();
        decoded->bfbs = schema_row.blob(0);
        decoded->hash = *value.schema;
        decoded->root_type = *value.root_type;
        type->second = std::move(decoded);
      }
      if (!crypto::base64_decode(*value.data_b64, v.bytes)) return std::nullopt;
      v.type = type->second;
      group.values.push_back(std::move(v));
    }
    li.groups.push_back(std::move(group));
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
    for (const auto& m : *list.items)
      // A copied history may be another's: a member is a path inside its artifact.
      if (fs::valid_relative_path(*m.path)) a.members.push_back({*m.path, *m.size, *m.hash});
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
    if (seqs) seqs->push_back(st.i64(12));  // after kRunCols
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

// ---------------------------------------------------------------------------
// The project's copy of finished runs

namespace {


// The copy: the run tables (same names and columns as the history, so runs
// move between the two with the same column lists, kCopied) and its format.
std::string copy_schema() {
  return "CREATE TABLE IF NOT EXISTS ex.meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);\n" + run_tables("ex.");
}

struct Copied {
  const char* table;
  const char* cols;
  const char* order;
  bool has_card = true;
};
// Parents first: a run is complete in the other database once it commits.
constexpr Copied kCopied[] = {
    {"runs",
     "run_id, op_id, state, rev, execution_rev, accepted_at, started_at, ended_at, failure, source_hash, "
     "target, propagate, has_artifacts",
     "seq", false},
    {"build_environments", "run_id, id, derivation, manifest, system, path_count", "rowid", false},
    {"environment_paths", "run_id, environment_id, position, path", "rowid", false},
    {"run_substituters", "run_id, position, url", "rowid", false},
    {"run_cards", "run_id, card", "rowid"},
    {"run_batches", "run_id, card, activation, invocation_count, execution_list, complete, failed, outputs", "rowid"},
    {"invocations",
     "run_id, card, invocation, state, phase, started_at, ended_at, duration_ms, failure, waiting, "
     "record_io, input_sources, display, build_cached, detail",
     "rowid"},
    {"ivalues",
     "run_id, card, invocation, side, port, kind, schema_hash, root_type, size, bytes, hash, artifact_id",
     "rowid"},
    {"logs", "run_id, card, invocation, stream, run_off, card_off, inv_off, data", "id"},
    {"artifacts", "artifact_id, run_id, card, invocation, output, kind, name, members", "rowid"},
    {"displays", "run_id, card, revision, data", "rowid"},
};

std::string finished_states() {
  return std::to_string(static_cast<int>(RunState::succeeded)) + "," +
         std::to_string(static_cast<int>(RunState::failed)) + "," +
         std::to_string(static_cast<int>(RunState::cancelled));
}

// The copy is changed under another name and put in place whole when done:
// it lives on the project share, where the Mac (and other programs) read it
// and cannot see this side's file locks, so a rollback journal left there
// during a write would look like a crashed writer to them.
std::filesystem::path staged(const std::filesystem::path& copy) { return copy.string() + ".new"; }

// This database with a staged copy attached as `ex` (made from the copy, or
// new when there is none). A copy written by a newer version is never
// changed. `put_copy` puts the staged copy in place.
std::unique_ptr<sql::Db> with_copy(const std::filesystem::path& db_path, const std::filesystem::path& copy) {
  std::error_code ec;
  auto fresh = staged(copy);
  std::filesystem::remove(fresh, ec);
  if (std::filesystem::exists(copy, ec)) {
    // Written by SQLite like a new database (a file copy of the system can
    // be refused on the project share).
    sql::Db f(":memory:");
    f.prepare("ATTACH DATABASE ? AS ex").bind(1, copy.string()).run();
    f.prepare("VACUUM ex INTO ?").bind(1, fresh.string()).run();
  } else {
    sql::Db f(":memory:");
    f.prepare("ATTACH DATABASE ? AS ex").bind(1, fresh.string()).run();
    f.exec("PRAGMA ex.journal_mode=DELETE");
    f.exec(copy_schema());
    f.prepare("INSERT OR IGNORE INTO ex.meta(key, value) VALUES('format', ?), ('version', ?)")
        .bind(1, kCopyFormat).bind(2, kCopyVersion).run();
  }
  auto db = std::make_unique<sql::Db>(db_path.string());
  db->prepare("ATTACH DATABASE ? AS ex").bind(1, fresh.string()).run();
  {
    auto st = db->prepare("SELECT 1 FROM ex.sqlite_master WHERE type='table' AND name='meta'");
    if (st.step()) {
      auto v = db->prepare("SELECT key, value FROM ex.meta WHERE key IN ('format','version')");
      std::map<std::string, std::string> meta;
      while (v.step()) meta[v.text(0)] = v.text(1);
      if (meta["format"] != kCopyFormat || meta["version"] != kCopyVersion)
        throw std::runtime_error(copy.string() + " is format " + meta["format"] + " version " + meta["version"] +
                                 ", not " + kCopyFormat + " " + kCopyVersion);
    }
  }
  // A plain rollback journal: the copy lives on the project share, where
  // other programs (and the Mac) may read it.
  db->exec("PRAGMA ex.journal_mode=DELETE");
  db->exec("PRAGMA ex.synchronous=FULL");
  db->exec(copy_schema());
  db->prepare("INSERT OR IGNORE INTO ex.meta(key, value) VALUES('format', ?), ('version', ?)")
      .bind(1, kCopyFormat).bind(2, kCopyVersion).run();
  return db;
}

// Attaches the copy read-only as `ex` (reading it never rewrites it).
void attach_copy(sql::Db& db, const std::filesystem::path& copy) {
  std::string uri = "file:";
  for (char c : copy.string()) {
    if (c == '%' || c == '?' || c == '#') {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", static_cast<unsigned char>(c));
      uri += b;
    } else {
      uri += c;
    }
  }
  db.prepare("ATTACH DATABASE ? AS ex").bind(1, uri + "?mode=ro").run();
  auto v = db.prepare("SELECT key, value FROM ex.meta WHERE key IN ('format','version')");
  std::map<std::string, std::string> meta;
  while (v.step()) meta[v.text(0)] = v.text(1);
  if (meta["format"] != kCopyFormat || meta["version"] != kCopyVersion)
    throw std::runtime_error(copy.string() + " is format " + meta["format"] + " version " + meta["version"] +
                             ", not " + kCopyFormat + " " + kCopyVersion);
}

// Detaches the staged copy and puts it in place of the copy (one rename).
void put_copy(sql::Db& db, const std::filesystem::path& copy) {
  db.exec("DETACH DATABASE ex");
  std::filesystem::rename(staged(copy), copy);
}

// BEGIN (deferred: reading one side never blocks the other's writers) / COMMIT.
class Txn {
 public:
  explicit Txn(sql::Db& db) : db_(db) { db_.exec("BEGIN"); }
  ~Txn() {
    if (!done_) {
      try {
        db_.exec("ROLLBACK");
      } catch (...) {
      }
    }
  }
  void commit() {
    db_.exec("COMMIT");
    done_ = true;
  }

 private:
  sql::Db& db_;
  bool done_ = false;
};

// Copies the finished runs `from` has and `to` lacks; inside a transaction.
// A run or artifact id names a folder: only UUIDs are taken from a copy.
const std::string kUuidGlob = [] {
  std::string h = "[0-9a-fA-F]";
  std::string g;
  for (int n : {8, 4, 4, 4, 12}) {
    if (!g.empty()) g += "-";
    for (int i = 0; i < n; ++i) g += h;
  }
  return "'" + g + "'";
}();

size_t copy_runs(sql::Db& db, const std::string& from, const std::string& to) {
  db.exec("CREATE TEMP TABLE IF NOT EXISTS moving(run_id TEXT PRIMARY KEY); DELETE FROM temp.moving");
  // A finished run can still have detached jobs to reconcile. Export only
  // after they have written their final invocation records.
  const std::string pending = from == "main" ? " AND run_id NOT IN (SELECT run_id FROM main.remote_jobs)" : "";
  db.exec("INSERT INTO temp.moving SELECT run_id FROM " + from + ".runs WHERE state IN (" + finished_states() +
          ") AND run_id GLOB " + kUuidGlob + " AND run_id NOT IN (SELECT run_id FROM " + to + ".runs)" +
          pending + " ORDER BY seq");
  size_t n = static_cast<size_t>(db.changes());
  if (n == 0) return 0;
  for (const auto& c : kCopied)
    db.exec(std::string("INSERT OR IGNORE INTO ") + to + "." + c.table + "(" + c.cols + ") SELECT " + c.cols +
            " FROM " + from + "." + c.table + " WHERE run_id IN (SELECT run_id FROM temp.moving) ORDER BY " + c.order);
  db.exec("INSERT OR IGNORE INTO " + to + ".schemas(hash, root_type, bfbs) SELECT hash, root_type, bfbs FROM " + from +
          ".schemas WHERE hash IN (SELECT schema_hash FROM " + from +
          ".ivalues WHERE run_id IN (SELECT run_id FROM temp.moving))");
  db.exec("DELETE FROM " + to + ".artifacts WHERE run_id IN (SELECT run_id FROM temp.moving) AND NOT artifact_id GLOB " +
          kUuidGlob);
  return n;
}

}  // namespace

size_t History::import_runs(const std::filesystem::path& copy) {
  std::lock_guard<std::mutex> l(copy_mu_);
  std::error_code ec;
  if (!std::filesystem::exists(copy, ec)) return 0;
  // Read where it is: importing changes this database only.
  sql::Db db(path_.string());
  attach_copy(db, copy);
  Txn t(db);
  size_t n = copy_runs(db, "ex", "main");
  t.commit();
  db.exec("DETACH DATABASE ex");
  return n;
}

size_t History::export_runs(const std::filesystem::path& copy) {
  std::lock_guard<std::mutex> l(copy_mu_);
  std::error_code ec;
  if (std::filesystem::exists(copy, ec)) {
    // Nothing new for the copy: it is left as it is (not written again).
    sql::Db probe(path_.string());
    attach_copy(probe, copy);
    bool none;
    {
      auto st = probe.prepare("SELECT count(*) FROM main.runs WHERE state IN (" + finished_states() +
                              ") AND run_id NOT IN (SELECT run_id FROM ex.runs) "
                              "AND run_id NOT IN (SELECT run_id FROM main.remote_jobs)");
      none = st.step() && st.i64(0) == 0;
    }
    probe.exec("DETACH DATABASE ex");
    if (none) return 0;
  }
  auto db = with_copy(path_, copy);
  Txn t(*db);
  size_t n = copy_runs(*db, "main", "ex");
  t.commit();
  put_copy(*db, copy);
  return n;
}

std::vector<std::string> History::delete_runs(const std::filesystem::path& copy, std::optional<int64_t> ended_before) {
  std::lock_guard<std::mutex> l(copy_mu_);
  std::error_code ec;
  bool has_copy = std::filesystem::exists(copy, ec);
  auto db = has_copy ? with_copy(path_, copy) : std::make_unique<sql::Db>(path_.string());
  std::vector<std::string> schemas = {"main"};
  if (has_copy) schemas.push_back("ex");
  std::vector<std::string> ids;
  {
    Txn t(*db);
    db->exec("CREATE TEMP TABLE IF NOT EXISTS moving(run_id TEXT PRIMARY KEY); DELETE FROM temp.moving");
    for (const auto& s : schemas) {
      auto st = db->prepare("INSERT OR IGNORE INTO temp.moving SELECT run_id FROM " + s + ".runs WHERE state IN (" +
                            finished_states() + ") AND (?1 IS NULL OR ended_at < ?1)");
      st.bind(1, ended_before).run();
    }
    db->exec("DELETE FROM temp.moving WHERE run_id IN (SELECT run_id FROM main.remote_jobs)");
    auto st = db->prepare("SELECT run_id FROM temp.moving");
    while (st.step()) ids.push_back(st.text(0));
    for (const auto& s : schemas) {
      for (const auto& c : kCopied)
        db->exec("DELETE FROM " + s + "." + c.table + " WHERE run_id IN (SELECT run_id FROM temp.moving)");
      if (s == "main") {
        db->exec("DELETE FROM main.statuses WHERE run_id IN (SELECT run_id FROM temp.moving)");
        db->exec("DELETE FROM main.active_environments WHERE run_id IN (SELECT run_id FROM temp.moving)");
      }
    }
    // Saved inputs here may still name types, so only the copy drops unused ones.
    if (has_copy) db->exec("DELETE FROM ex.schemas WHERE hash NOT IN (SELECT schema_hash FROM ex.ivalues)");
    t.commit();
  }
  if (!ids.empty()) {
    // The space goes back to the disk; a failure here loses nothing.
    for (const auto& s : schemas) {
      try {
        db->exec("VACUUM " + s);
      } catch (const std::exception& e) {
        log::warn("history: VACUUM " + s + " failed: " + e.what());
      }
    }
    try {
      db->exec("PRAGMA main.wal_checkpoint(TRUNCATE)");
    } catch (const std::exception&) {
    }
  }
  if (has_copy) put_copy(*db, copy);
  return ids;
}


}  // namespace tsugumi
