#pragma once

#include <mutex>
#include <string>
#include <utility>

#include "server.hh"
#include "util/sqlite.hh"

namespace tsugumi::host {
struct SkillsService {
  explicit SkillsService(sql::Db& db);
  auto Handle(const RequestIn& request, bool human,
              std::unique_lock<std::recursive_mutex>& lock) -> std::string;
  sql::Db& db;
};
}  // namespace tsugumi::host
