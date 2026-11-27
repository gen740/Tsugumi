#pragma once

#include <sys/types.h>

#include <filesystem>
#include <string>
#include <vector>

namespace tsugumi::host {

// Owned by the execution broker. The broker serializes startup and updates to
// the allowlist with registration changes.
class CacheProcess {
 public:
  struct Endpoint {
    std::string port;
    std::string signing_key;
    std::string tls_key;
  };

  CacheProcess(std::filesystem::path state, std::filesystem::path executable);
  ~CacheProcess();
  CacheProcess(const CacheProcess&) = delete;
  auto operator=(const CacheProcess&) -> CacheProcess& = delete;

  auto Start(const std::vector<std::string>& public_keys) -> Endpoint;
  auto UpdateKeys(const std::vector<std::string>& public_keys) -> void;

 private:
  auto Stop() -> void;

  std::filesystem::path state_;
  std::filesystem::path executable_;
  Endpoint endpoint_;
  std::vector<std::string> allowed_keys_;
  pid_t pid_ = -1;
  int lifetime_ = -1;
};

}  // namespace tsugumi::host
