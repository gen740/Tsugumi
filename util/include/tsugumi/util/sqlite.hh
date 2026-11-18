// Minimal RAII wrapper over sqlite3.
#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tsugumi::sql {

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Stmt {
 public:
  Stmt(sqlite3* db, std::string_view sql);
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  Stmt(Stmt&& o) noexcept : db_(o.db_), s_(o.s_) { o.s_ = nullptr; }
  ~Stmt();

  Stmt& bind(int i, int64_t v);
  Stmt& bind(int i, uint64_t v) { return bind(i, static_cast<int64_t>(v)); }
  Stmt& bind(int i, int v) { return bind(i, static_cast<int64_t>(v)); }
  Stmt& bind(int i, bool v) { return bind(i, static_cast<int64_t>(v ? 1 : 0)); }
  Stmt& bind(int i, std::string_view v);
  Stmt& bind(int i, const std::string& v) { return bind(i, std::string_view(v)); }
  Stmt& bind(int i, const char* v) { return bind(i, std::string_view(v)); }
  Stmt& bind_blob(int i, std::string_view v);
  Stmt& bind_null(int i);
  template <class T>
  Stmt& bind(int i, const std::optional<T>& v) {
    if (v) return bind(i, *v);
    return bind_null(i);
  }

  // Returns true when a row is available.
  bool step();
  void run() {
    while (step()) {}
  }
  void reset();

  bool is_null(int c) const;
  int64_t i64(int c) const;
  uint64_t u64(int c) const { return static_cast<uint64_t>(i64(c)); }
  std::optional<int64_t> opt_i64(int c) const;
  std::optional<uint64_t> opt_u64(int c) const;
  std::string text(int c) const;
  std::string blob(int c) const;

 private:
  sqlite3* db_;
  sqlite3_stmt* s_ = nullptr;
};

class Db {
 public:
  explicit Db(const std::string& path, bool readonly = false);
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;
  ~Db();
  sqlite3* get() const { return db_; }
  void exec(std::string_view sql);
  Stmt prepare(std::string_view sql) { return Stmt(db_, sql); }
  int changes() const { return sqlite3_changes(db_); }

 private:
  sqlite3* db_ = nullptr;
};

// BEGIN IMMEDIATE / COMMIT, rolls back on destruction without commit.
class Transaction {
 public:
  explicit Transaction(Db& db, bool write = true);
  ~Transaction();
  void commit();

 private:
  Db& db_;
  bool done_ = false;
};

}  // namespace tsugumi::sql
