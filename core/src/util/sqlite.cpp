#include "util/sqlite.hpp"

namespace tsugumi::sql {

namespace {
[[noreturn]] void fail(sqlite3* db, const std::string& what) {
  throw Error(what + ": " + (db ? sqlite3_errmsg(db) : "sqlite error"));
}
}  // namespace

Stmt::Stmt(sqlite3* db, std::string_view sql) : db_(db) {
  if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &s_, nullptr) != SQLITE_OK)
    fail(db, "prepare " + std::string(sql.substr(0, 60)));
}

Stmt::~Stmt() {
  if (s_) sqlite3_finalize(s_);
}

Stmt& Stmt::bind(int i, int64_t v) {
  if (sqlite3_bind_int64(s_, i, v) != SQLITE_OK) fail(db_, "bind");
  return *this;
}

Stmt& Stmt::bind(int i, std::string_view v) {
  if (sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT) != SQLITE_OK)
    fail(db_, "bind");
  return *this;
}

Stmt& Stmt::bind_blob(int i, std::string_view v) {
  if (sqlite3_bind_blob(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT) != SQLITE_OK)
    fail(db_, "bind");
  return *this;
}

Stmt& Stmt::bind_null(int i) {
  if (sqlite3_bind_null(s_, i) != SQLITE_OK) fail(db_, "bind");
  return *this;
}

bool Stmt::step() {
  int rc = sqlite3_step(s_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  fail(db_, "step");
}

void Stmt::reset() {
  sqlite3_reset(s_);
  sqlite3_clear_bindings(s_);
}

bool Stmt::is_null(int c) const { return sqlite3_column_type(s_, c) == SQLITE_NULL; }
int64_t Stmt::i64(int c) const { return sqlite3_column_int64(s_, c); }
std::optional<int64_t> Stmt::opt_i64(int c) const {
  if (is_null(c)) return std::nullopt;
  return i64(c);
}
std::optional<uint64_t> Stmt::opt_u64(int c) const {
  if (is_null(c)) return std::nullopt;
  return u64(c);
}
std::string Stmt::text(int c) const {
  const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s_, c));
  return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(s_, c))) : std::string();
}
std::string Stmt::blob(int c) const {
  const auto* p = static_cast<const char*>(sqlite3_column_blob(s_, c));
  return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(s_, c))) : std::string();
}

Db::Db(const std::string& path, bool readonly) {
  int flags = readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  flags |= SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : "open failed";
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    throw Error("open " + path + ": " + msg);
  }
  sqlite3_busy_timeout(db_, 10000);
}

Db::~Db() {
  if (db_) sqlite3_close_v2(db_);
}

void Db::exec(std::string_view sql) {
  char* err = nullptr;
  std::string s(sql);
  if (sqlite3_exec(db_, s.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "exec failed";
    sqlite3_free(err);
    throw Error(msg);
  }
}

Transaction::Transaction(Db& db, bool write) : db_(db) {
  db_.exec(write ? "BEGIN IMMEDIATE" : "BEGIN");
}

Transaction::~Transaction() {
  if (!done_) {
    try {
      db_.exec("ROLLBACK");
    } catch (...) {
    }
  }
}

void Transaction::commit() {
  db_.exec("COMMIT");
  done_ = true;
}

}  // namespace tsugumi::sql
