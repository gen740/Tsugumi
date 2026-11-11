#include "database.hh"

#include <fcntl.h>
#include <libpq-fe.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>

#include "schema.hh"
#include "util/fs.hh"
#include "util/sqlite.hh"

extern char** environ;

namespace tsugumi::database {
namespace {
namespace stdfs = std::filesystem;

struct Credential {
  schema::t::String<"url"> url;
  schema::t::String<"user"> user;
  schema::t::String<"password"> password;
};

[[noreturn]] auto fail(const char* message) -> void {
  throw std::runtime_error(message);
}

// Reject userinfo, query parameters other than sslmode, Unix sockets and host
// lists. No secrets or user-controlled libpq options enter project history.
auto postgres_options(const std::string& url)
    -> std::map<std::string, std::string> {
  if (!url.starts_with("postgresql://") || url.find('\0') != std::string::npos)
    fail(
        "database URL must use sqlite:FILE.sqlite or "
        "postgresql://HOST/DATABASE");
  char* error = nullptr;
  auto* raw = PQconninfoParse(url.c_str(), &error);
  std::unique_ptr<char, decltype(&PQfreemem)> message(error, PQfreemem);
  if (!raw)
    fail(
        "invalid PostgreSQL URL (credentials must be stored outside the "
        "project)");
  std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)> options(
      raw, PQconninfoFree);
  std::map<std::string, std::string> result;
  for (auto* item = raw; item->keyword; ++item) {
    if (!item->val) continue;
    const std::string key = item->keyword;
    if (key != "host" && key != "port" && key != "dbname" && key != "sslmode")
      fail(
          "PostgreSQL URL permits only host, port, database and sslmode; "
          "store credentials separately");
    result[key] = item->val;
  }
  for (auto key : {"host", "dbname"})
    if (result[key].empty())
      fail("PostgreSQL URL needs an explicit host and database");
  if (result["host"].find_first_of("/\\, \t\r\n") != std::string::npos)
    fail("PostgreSQL URL requires one TCP host");
  if (result["port"].empty()) result["port"] = "5432";
  if (result["port"].size() > 5 ||
      result["port"].find_first_not_of("0123456789") != std::string::npos ||
      std::stoi(result["port"]) < 1 || std::stoi(result["port"]) > 65535)
    fail("PostgreSQL port must be between 1 and 65535");
  if (result["sslmode"].empty()) result["sslmode"] = "verify-full";
  if (result["sslmode"] != "verify-full" && result["sslmode"] != "disable")
    fail("PostgreSQL sslmode must be verify-full or disable");
  return result;
}

auto sqlite_name(const std::string& url) -> std::string {
  const auto name = url.substr(7);
  if (!name.ends_with(".sqlite") ||
      !valid_table(name.substr(0, name.size() - 7)))
    fail("SQLite URL must be sqlite:NAME.sqlite (stored in .tsugumi/logger)");
  return name;
}

// This directory belongs to the launcher, never to project documents, the
// Agent catalog, run records or version snapshots. Bind credentials to the
// exact public target so changing a URL cannot forward an existing password.
auto credential(const stdfs::path& project, const stdfs::path& directory,
                const std::string& name, const std::string& url) -> Credential {
  if (directory.empty() || fs::resolves_inside(project, directory))
    fail("configure a database credentials directory outside the project");
  const auto file = directory / (name + ".json");
  int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) fail("database credential file is missing or inaccessible");
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } close{fd};
  struct stat st{}, parent{};
  if (::stat(directory.c_str(), &parent) != 0)
    fail("invalid database credentials directory");
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != parent.st_uid || (st.st_mode & 0777) != 0600 ||
      st.st_size < 1 || st.st_size > 65536)
    fail(
        "database credential file must be an owner-only regular file with "
        "mode 0600");
  std::string bytes(static_cast<size_t>(st.st_size), '\0');
  size_t offset = 0;
  while (offset < bytes.size()) {
    auto got = ::read(fd, bytes.data() + offset, bytes.size() - offset);
    if (got <= 0) fail("cannot read database credential file");
    offset += static_cast<size_t>(got);
  }
  Credential c;
  try {
    if (schema::decode(bytes, c)) fail("invalid database credential file");
  } catch (...) {
    fail("invalid database credential file");
  }
  if (*c.url != url || c.user->empty() || c.password->empty() ||
      c.user->find('\0') != std::string::npos ||
      c.password->find('\0') != std::string::npos)
    fail(
        "database credentials do not match the connection target or are "
        "incomplete");
  return c;
}

auto check_directory(const stdfs::path& path) -> void {
  std::error_code ec;
  stdfs::create_directory(path, ec);
  const auto status = stdfs::symlink_status(path, ec);
  if (ec || !stdfs::is_directory(status) || stdfs::is_symlink(status))
    fail("logger database directory must be a real project directory");
}

auto sqlite_write(const stdfs::path& project, const Node& node,
                  const std::string& run, uint64_t invocation,
                  const std::string& value, uint64_t timestamp) -> void {
  // Only a dedicated state directory is writable: never arbitrary source or
  // history files. SQLite's NOFOLLOW also rejects a symlink at open time.
  auto directory = project / node.project_dir / ".tsugumi";
  if (!fs::resolves_inside(project, directory))
    fail("logger database is outside the project");
  check_directory(directory);
  directory /= "logger";
  check_directory(directory);
  auto file = directory / sqlite_name(node.settings.database_url);
  for (const auto* suffix : {"", "-journal", "-wal", "-shm"}) {
    std::error_code ec;
    const auto status = stdfs::symlink_status(file.string() + suffix, ec);
    if (stdfs::is_symlink(status))
      fail("logger database cannot use symbolic links");
  }
  sqlite3* raw = nullptr;
  const int opened = sqlite3_open_v2(
      file.c_str(), &raw,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW,
      nullptr);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
  if (opened != SQLITE_OK) fail("cannot open logger SQLite database");
  sqlite3_busy_timeout(raw, 5000);
  const auto table = "\"" + node.settings.database_table + "\"";
  try {
    sql::Stmt create(raw, "CREATE TABLE IF NOT EXISTS " + table +
                              " (run_id TEXT NOT NULL, card TEXT NOT NULL, "
                              "invocation INTEGER NOT NULL,"
                              " timestamp_ms INTEGER NOT NULL, value_json TEXT "
                              "NOT NULL CHECK(json_valid(value_json)),"
                              " PRIMARY KEY(run_id, card, invocation))");
    create.run();
    sql::Stmt insert(raw, "INSERT INTO " + table + " VALUES(?,?,?,?,?)");
    insert.bind(1, run)
        .bind(2, path_key(node.path))
        .bind(3, invocation)
        .bind(4, timestamp)
        .bind(5, value)
        .run();
  } catch (...) {
    fail(
        "logger SQLite write failed (check table schema, permissions and disk "
        "space)");
  }
}

auto postgres_write(const stdfs::path& project, const stdfs::path& directory,
                    const Node& node, const std::string& run,
                    uint64_t invocation, const std::string& value,
                    uint64_t timestamp) -> void {
  if (!node.settings.network)
    fail("PostgreSQL logging requires network permission");
  // libpq reads PG* variables as implicit connection options. Do not let those
  // redirect a public target or select another password/certificate source.
  for (char** env = environ; env && *env; ++env)
    if (std::string_view(*env).starts_with("PG"))
      fail("remove PG* environment overrides from the logger daemon");
  auto options = postgres_options(node.settings.database_url);
  auto secret =
      credential(project, directory, node.settings.database_connection,
                 node.settings.database_url);
  options["user"] = *secret.user;
  options["password"] = *secret.password;
  options["passfile"] = "/dev/null";
  options["connect_timeout"] = "5";
  options["options"] = "-c statement_timeout=5000 -c lock_timeout=5000";
  options["sslcertmode"] = "disable";
  if (options["sslmode"] == "verify-full") options["sslrootcert"] = "system";
  options["gssencmode"] = "disable";
  std::vector<const char*> keys, values;
  for (const auto& [key, value] : options) {
    keys.push_back(key.c_str());
    values.push_back(value.c_str());
  }
  keys.push_back(nullptr);
  values.push_back(nullptr);
  std::unique_ptr<PGconn, decltype(&PQfinish)> db(
      PQconnectdbParams(keys.data(), values.data(), 0), PQfinish);
  if (!db || PQstatus(db.get()) != CONNECTION_OK)
    fail("logger PostgreSQL connection failed");
  auto exec = [&](const std::string& query,
                  const std::vector<const char*>& params) {
    std::unique_ptr<PGresult, decltype(&PQclear)> result(
        PQexecParams(db.get(), query.c_str(), static_cast<int>(params.size()),
                     nullptr, params.data(), nullptr, nullptr, 0),
        PQclear);
    // Server text can echo values or secrets. Return only a fixed diagnostic.
    if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK)
      fail(
          "logger PostgreSQL write failed (check table schema and database "
          "permissions)");
  };
  const auto table = "\"" + node.settings.database_table + "\"";
  exec("CREATE TABLE IF NOT EXISTS " + table +
           " (run_id TEXT NOT NULL, card TEXT NOT NULL, invocation BIGINT NOT "
           "NULL,"
           " timestamp_ms BIGINT NOT NULL, value_json JSONB NOT NULL, PRIMARY "
           "KEY(run_id, card, invocation))",
       {});
  const auto card = path_key(node.path), inv = std::to_string(invocation),
             time = std::to_string(timestamp);
  exec("INSERT INTO " + table + " VALUES($1,$2,$3,$4,$5::jsonb)",
       {run.c_str(), card.c_str(), inv.c_str(), time.c_str(), value.c_str()});
}
}  // namespace

auto valid_table(std::string_view name) -> bool {
  if (name.empty() || name.size() > 63 ||
      (name.front() >= '0' && name.front() <= '9'))
    return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  });
}

auto validate_url(const std::string& url) -> void {
  if (url.starts_with("sqlite:")) {
    sqlite_name(url);
    return;
  }
  postgres_options(url);
}

auto write(const stdfs::path& project, const stdfs::path& credentials,
           const Node& node, const std::string& run, uint64_t invocation,
           const std::string& value, uint64_t timestamp_ms) -> void {
  validate_url(node.settings.database_url);
  if (!valid_table(node.settings.database_table))
    fail("invalid logger database table");
  if (node.settings.database_url.starts_with("sqlite:"))
    sqlite_write(project, node, run, invocation, value, timestamp_ms);
  else
    postgres_write(project, credentials, node, run, invocation, value,
                   timestamp_ms);
}
}  // namespace tsugumi::database
