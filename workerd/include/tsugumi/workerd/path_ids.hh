#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tsugumi/util/sha256_hex.hh"
#include "tsugumi/workerd/protocol.hh"

namespace tsugumi::workerd {
inline auto require_sha256_hex(std::string_view text, std::string_view field)
    -> Sha256Hex {
  if (const auto id = Sha256Hex::parse(text)) return *id;
  throw Error(R::ErrorCode::Invalid, "invalid " + std::string(field));
}

inline auto parse_input_blobs(const R::JobSpecT& spec)
    -> std::vector<Sha256Hex> {
  std::vector<Sha256Hex> blobs;
  blobs.reserve(spec.input_blobs.size());
  for (const auto& digest : spec.input_blobs)
    blobs.push_back(require_sha256_hex(digest, "input blob digest"));
  return blobs;
}
}  // namespace tsugumi::workerd
