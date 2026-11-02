// Text helpers shared by the core and the worker.
#pragma once

#include <string_view>

namespace tsugumi::text {

// Returns true when `s` is valid UTF-8 (no surrogates, no overlongs).
bool valid_utf8(std::string_view s);

}  // namespace tsugumi::text
