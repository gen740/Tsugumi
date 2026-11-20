#include "host/store.hh"

#include <sys/stat.h>

namespace tsugumi::host {
HostStore::HostStore(const std::filesystem::path& directory)
    : db((directory / "host.sqlite").string()) {
  ::chmod((directory / "host.sqlite").c_str(), 0600);
  db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
  db.exec(
      "CREATE TABLE IF NOT EXISTS identifiers(scope TEXT NOT NULL, id TEXT "
      "NOT NULL, PRIMARY KEY(scope,id))");
  db.exec(
      "CREATE TABLE IF NOT EXISTS skill_trust(path TEXT NOT NULL, "
      "recursive INTEGER NOT NULL, device INTEGER NOT NULL, "
      "inode INTEGER NOT NULL, decision INTEGER NOT NULL, "
      "PRIMARY KEY(path,recursive))");
  db.exec(
      "CREATE TABLE IF NOT EXISTS projects(id TEXT PRIMARY KEY, endpoint "
      "TEXT NOT NULL, token TEXT NOT NULL, title TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS folders(id TEXT PRIMARY KEY, project TEXT "
      "NOT NULL, path TEXT NOT NULL, title TEXT NOT NULL, "
      "UNIQUE(project,path));"
      "CREATE TABLE IF NOT EXISTS permission_requests(id TEXT PRIMARY KEY, "
      "project TEXT NOT NULL, conversation TEXT NOT NULL, access INTEGER NOT "
      "NULL, reason TEXT NOT NULL, state TEXT NOT NULL, call TEXT, digest "
      "TEXT, "
      "offered INTEGER NOT NULL DEFAULT 0, UNIQUE(conversation,access,call));"
      "CREATE TABLE IF NOT EXISTS workspace_grants(id TEXT PRIMARY KEY, "
      "project TEXT "
      "NOT NULL, access INTEGER NOT NULL, active "
      "INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS providers(conversation TEXT PRIMARY KEY, "
      "json TEXT NOT NULL);");
  db.exec(
      "CREATE TABLE IF NOT EXISTS setting_values(scope TEXT NOT NULL, "
      "key TEXT NOT NULL, value TEXT NOT NULL, PRIMARY KEY(scope,key));"
      "CREATE TABLE IF NOT EXISTS settings_revision(revision INTEGER NOT NULL,"
      "store TEXT NOT NULL);"
      "INSERT INTO settings_revision SELECT 0,lower(hex(randomblob(16))) "
      "WHERE NOT EXISTS "
      "(SELECT 1 FROM settings_revision);"
      "CREATE TABLE IF NOT EXISTS provider_connections(profile TEXT,id TEXT,"
      "json TEXT NOT NULL,PRIMARY KEY(profile,id));"
      "CREATE TABLE IF NOT EXISTS accounts(profile TEXT,id TEXT,json TEXT "
      "NOT NULL,PRIMARY KEY(profile,id));"
      "CREATE TABLE IF NOT EXISTS oauth_hosts(profile TEXT PRIMARY KEY,id "
      "TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS credentials(id TEXT PRIMARY KEY, json "
      "TEXT NOT NULL)");
  db.exec(
      "CREATE TABLE IF NOT EXISTS conversation_locations(conversation TEXT "
      "PRIMARY KEY, project TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS environment_projects(key TEXT PRIMARY KEY, "
      "project TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS project_attached_roots(project TEXT NOT "
      "NULL, "
      "path TEXT NOT NULL, execution_path TEXT NOT NULL, PRIMARY "
      "KEY(project,path));"
      "CREATE TABLE IF NOT EXISTS project_paths(id TEXT PRIMARY KEY, path "
      "TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS skill_catalogs(conversation TEXT PRIMARY "
      "KEY, path TEXT NOT NULL, folders TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS model_lists(id TEXT NOT NULL, connection "
      "TEXT NOT NULL, models TEXT NOT NULL, PRIMARY KEY(id,connection));");
}
}  // namespace tsugumi::host
