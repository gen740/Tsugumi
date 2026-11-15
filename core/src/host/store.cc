#include "host/store.hh"

#include <sys/stat.h>

namespace tsugumi::host {
HostStore::HostStore(const std::filesystem::path& directory)
    : db((directory / "host.sqlite").string()) {
  ::chmod((directory / "host.sqlite").c_str(), 0600);
  db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
  db.exec("CREATE TABLE IF NOT EXISTS project_trust(path TEXT PRIMARY KEY, "
          "device INTEGER NOT NULL, inode INTEGER NOT NULL, "
      "decision INTEGER NOT NULL)");
  db.exec(
      "CREATE TABLE IF NOT EXISTS projects(id TEXT PRIMARY KEY, endpoint "
      "TEXT NOT NULL, token TEXT NOT NULL, title TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS folders(id TEXT PRIMARY KEY, project TEXT "
      "NOT NULL, path TEXT NOT NULL, title TEXT NOT NULL, "
      "UNIQUE(project,path));"
      "CREATE TABLE IF NOT EXISTS access_requests(id TEXT PRIMARY KEY, "
      "project TEXT NOT NULL, conversation TEXT NOT NULL, access INTEGER NOT "
      "NULL, scope INTEGER NOT NULL, reason TEXT NOT NULL, state TEXT NOT "
      "NULL);"
      "CREATE TABLE IF NOT EXISTS grants(id TEXT PRIMARY KEY, project TEXT "
      "NOT NULL, conversation TEXT NOT NULL, access INTEGER NOT NULL, active "
      "INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS providers(conversation TEXT PRIMARY KEY, "
      "json TEXT NOT NULL);");
  db.exec(
      "CREATE TABLE IF NOT EXISTS save_bases(project TEXT NOT NULL, rev TEXT "
      "NOT NULL, "
      "snapshot TEXT NOT NULL, files TEXT NOT NULL, PRIMARY KEY(project,rev))");
  db.exec(
      "CREATE TABLE IF NOT EXISTS save_preparations(operation TEXT PRIMARY "
      "KEY, arguments TEXT NOT NULL, comments TEXT NOT NULL)");
  db.exec(
      "CREATE TABLE IF NOT EXISTS access_prompt_items(request TEXT NOT NULL, "
      "access INTEGER NOT NULL, "
      "PRIMARY KEY(request,access))");
  db.exec(
      "CREATE TABLE IF NOT EXISTS operation_approvals("
      "conversation TEXT NOT NULL, call TEXT NOT NULL, digest TEXT NOT NULL, "
      "request TEXT NOT NULL UNIQUE, preview TEXT NOT NULL, "
      "PRIMARY KEY(conversation,call))");
  db.exec(
      "CREATE TABLE IF NOT EXISTS credentials(id TEXT PRIMARY KEY, json "
      "TEXT NOT NULL)");
  db.exec(
      "CREATE TABLE IF NOT EXISTS environment_projects(key TEXT PRIMARY KEY, "
      "project TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS project_attached_roots(project TEXT NOT NULL, "
      "path TEXT NOT NULL, execution_path TEXT NOT NULL, PRIMARY KEY(project,path));"
      "CREATE TABLE IF NOT EXISTS project_paths(id TEXT PRIMARY KEY, path "
      "TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS skill_catalogs(conversation TEXT PRIMARY "
      "KEY, path TEXT NOT NULL, folders TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS model_lists(id TEXT NOT NULL, connection "
      "TEXT NOT NULL, models TEXT NOT NULL, PRIMARY KEY(id,connection));");
}
}  // namespace tsugumi::host
