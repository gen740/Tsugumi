#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/sqlite.hh"

namespace tsugumi::host {
// Call with the host StateGate held. Reservations survive deleted receipts and
// conversations; they contain only the identifier itself, never an alias.
inline auto
AllocateId(sql::Db &db, const std::string &scope,
           const std::function<std::string()> &generate = crypto::short_id)
    -> std::string {
  auto id = crypto::unique_short_id(
      [&](const auto &value) {
        auto q = db.prepare("SELECT 1 FROM identifiers WHERE scope=? AND id=?");
        q.bind(1, scope).bind(2, value);
        return q.step();
      },
      generate);
  db.prepare("INSERT INTO identifiers VALUES(?,?)")
      .bind(1, scope)
      .bind(2, id)
      .run();
  return id;
}
// Occupied document references and issued references use the same namespace.
// The state gate serializes independent Mac/Web windows before their edits.
inline auto ReserveOrdinal(sql::Db &db, const std::string &scope,
                           const std::string &prefix,
                           const std::vector<std::string> &occupied)
    -> std::string {
  auto reserve = db.prepare("INSERT OR IGNORE INTO identifiers VALUES(?,?)");
  for (const auto &id : occupied) {
    reserve.bind(1, scope).bind(2, id).run();
    reserve.reset();
  }
  auto count = db.prepare("SELECT count(*) FROM identifiers WHERE scope=?");
  count.bind(1, scope);
  count.step();
  for (uint64_t number = count.i64(0) + 1; number != 0; ++number) {
    const auto id = prefix + std::to_string(number);
    reserve.bind(1, scope).bind(2, id).run();
    reserve.reset();
    if (db.changes())
      return id;
  }
  throw std::runtime_error("annotation reference numbers exhausted");
}
} // namespace tsugumi::host
