#pragma once

#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "host/state_gate.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/sqlite.hh"

namespace tsugumi::host {
struct Credentials {
  Credentials(StateGate& gate, sql::Db& db, std::stop_token stop);
  auto Import(const std::string& id, const ::tsugumi::json::Members& value)
      -> void;
  auto Get(const std::string& id) -> ::tsugumi::json::Members;
  auto Save(const std::string& id, const ::tsugumi::json::Members& value)
      -> void;
  auto AccessToken(const std::string& id, std::string_view stale)
      -> std::string;
  auto Forget(const std::string& id) -> bool;
  auto Secrets() -> std::vector<std::string>;

  StateGate& gate;
  sql::Db& db;
  std::stop_token stop;
  std::mutex credential_mutex;
};
}  // namespace tsugumi::host
