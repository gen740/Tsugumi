#pragma once

namespace tsugumi {

// The native Nix system of this machine.
constexpr const char* native_system() {
#if defined(__aarch64__) && defined(__APPLE__)
  return "aarch64-darwin";
#elif defined(__aarch64__)
  return "aarch64-linux";
#elif defined(__x86_64__) && defined(__APPLE__)
  return "x86_64-darwin";
#else
  return "x86_64-linux";
#endif
}

}  // namespace tsugumi
