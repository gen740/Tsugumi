#pragma once

#include <mutex>

namespace tsugumi::host {
// The original single recursive state mutex. Components share this gate;
// extracting them must not narrow a request's existing serialization scope.
struct StateGate {
  StateGate();
  std::recursive_mutex mutex;
};
}  // namespace tsugumi::host
