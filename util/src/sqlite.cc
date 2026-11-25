#include "tsugumi/util/sqlite.hh"

#include <fcntl.h>

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <system_error>
#include <vector>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi::sql {

namespace {
[[noreturn]] void fail(sqlite3* db, const std::string& what) {
  throw Error(what + ": " + (db ? sqlite3_errmsg(db) : "sqlite error"));
}
auto rename_without_replacing(const std::filesystem::path& from,
                              const std::filesystem::path& to) -> void {
#ifdef __APPLE__
  const auto result = ::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#else
  const auto result = ::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(),
                                  RENAME_NOREPLACE);
#endif
  if (result != 0)
    throw std::filesystem::filesystem_error(
        "Cannot move state file aside", from, to,
        std::error_code(errno, std::generic_category()));
}
}  // namespace

FormatMismatch::FormatMismatch(std::filesystem::path path_, int64_t found_,
                               int expected_)
    : Error("This state was written by a different Tsugumi version. " +
            path_.string() + " (format " + std::to_string(found_) +
            ", expected " + std::to_string(expected_) + ")"),
      path(std::move(path_)),
      found(found_),
      expected(expected_) {}

auto move_aside(const FormatMismatch& mismatch,
                const std::filesystem::path& companion) -> void {
  namespace f = std::filesystem;
  const auto& path = mismatch.path;
  if (!f::is_regular_file(f::symlink_status(path)))
    throw Error("Expected a regular state file: " + path.string());

  for (const auto* suffix : {"-journal", "-wal", "-shm"})
    if (f::is_symlink(f::symlink_status(path.string() + suffix)))
      throw Error("State sidecars must not be symbolic links: " +
                  path.string());

  // Recheck after confirmation, in case another instance replaced the store.
  try {
    Db db(path.string(), true);
    db.check_format(mismatch.expected);
    throw Error("The state file no longer has an incompatible format: " +
                path.string());
  } catch (const FormatMismatch& current) {
    if (current.found != mismatch.found)
      throw Error("The state file changed after confirmation: " +
                  path.string());
  }

  const auto now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  char date[20]{};
  std::strftime(date, sizeof(date), "%Y%m%dT%H%M%SZ", &utc);
  const auto stamp = std::string(".stale-") + date + "-" + crypto::uuid4();
  const auto backup = path.string() + stamp;
  std::vector<std::pair<f::path, f::path>> files;
  if (!companion.empty()) {
    const auto entry = f::symlink_status(companion);
    if (f::exists(entry)) {
      if (!f::is_directory(entry))
        throw Error("Expected a state directory: " + companion.string());
      files.emplace_back(companion, companion.string() + stamp);
    }
  }
  for (const auto* suffix : {"-journal", "-wal", "-shm", ""}) {
    const f::path from = path.string() + suffix;
    const auto entry = f::symlink_status(from);
    if (!f::exists(entry) && *suffix) continue;
    const f::path to = backup + suffix;
    if (!f::is_regular_file(entry) || f::exists(f::symlink_status(to)))
      throw Error("Cannot move state file aside: " + from.string());
    files.emplace_back(from, to);
  }
  size_t moved = 0;
  try {
    for (const auto& [from, to] : files) {
      rename_without_replacing(from, to);
      ++moved;
    }
  } catch (...) {
    while (moved) {
      const auto& [from, to] = files[--moved];
      rename_without_replacing(to, from);
    }
    throw;
  }
  fs::fsync_dir(path.parent_path());
}

Stmt::Stmt(sqlite3* db, std::string_view sql) : db_(db) {
  if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &s_,
                         nullptr) != SQLITE_OK)
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
  if (sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK)
    fail(db_, "bind");
  return *this;
}

Stmt& Stmt::bind_blob(int i, std::string_view v) {
  if (sqlite3_bind_blob(s_, i, v.data(), static_cast<int>(v.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK)
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

bool Stmt::is_null(int c) const {
  return sqlite3_column_type(s_, c) == SQLITE_NULL;
}
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
  return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(s_, c)))
           : std::string();
}
std::string Stmt::blob(int c) const {
  const auto* p = static_cast<const char*>(sqlite3_column_blob(s_, c));
  return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(s_, c)))
           : std::string();
}

Db::Db(const std::string& path, bool readonly) {
  int flags = readonly ? SQLITE_OPEN_READONLY
                       : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  flags |= SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI;  // file:...?mode=ro attaches
  if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : "open failed";
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    throw Error("open " + path + ": " + msg);
  }
  // SQLite exposes no cross-process lock-release or cancellation event here.
  // Bound how long it can hold this call when the lock owner makes no progress.
  sqlite3_busy_timeout(db_, 10000);
  // A database may come from a copied project: no schema-defined code runs
  // and its schema cannot be written around SQLite.
  sqlite3_db_config(db_, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
  sqlite3_db_config(db_, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
}

Db::Db(const std::string& path, FormatVersion format)
    : Db([&] {
        // Closing an incompatible writer could checkpoint its WAL. Check
        // before opening any writer while the caller holds ownership.
        if (std::filesystem::exists(path)) {
          Db reader(path, true);
          reader.check_format(format.value);
        }
        return path;
      }()) {}

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

auto Db::has_format(int expected) -> bool {
  auto version = prepare("PRAGMA user_version");
  if (!version.step()) throw Error("SQLite did not return a format version");
  const auto found = version.i64(0);
  if (found == expected) return true;
  if (found == 0 && !prepare("SELECT 1 FROM sqlite_schema LIMIT 1").step())
    return false;
  throw FormatMismatch(sqlite3_db_filename(db_, "main"), found, expected);
}

auto Db::check_format(int expected) -> void { has_format(expected); }

auto Db::initialize_format(int expected, const std::function<void()>& create)
    -> void {
  Transaction transaction(*this);
  if (!has_format(expected)) {
    create();
    exec("PRAGMA user_version=" + std::to_string(expected));
  }
  transaction.commit();
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
