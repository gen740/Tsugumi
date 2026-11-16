#pragma once

#include <mutex>
#include <string>
#include <utility>

#include "server.hh"
#include "util/sqlite.hh"

namespace tsugumi::host {
class Settings;
struct SkillsService {
  SkillsService(sql::Db& db, Settings& settings);
  auto Handle(const RequestIn& request, bool human,
              std::unique_lock<std::recursive_mutex>& lock) -> std::string;
  sql::Db& db;
  Settings& settings;
};
}  // namespace tsugumi::host
