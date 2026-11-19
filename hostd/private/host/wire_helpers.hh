#pragma once

#include <flatbuffers/flatbuffers.h>

#include "host/failure.hh"

namespace tsugumi::host {
inline auto Text(const flatbuffers::String* value) -> std::string {
  return value ? value->str() : "";
}
inline auto Human(bool human) -> void {
  if (!human)
    throw Failure("permission_denied",
                  "This operation requires the authenticated user");
}
}  // namespace tsugumi::host
