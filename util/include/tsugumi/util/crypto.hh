// Hashing, encoding and identifier helpers.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace tsugumi::crypto {

// SHA-256 of the platform (CommonCrypto on macOS, OpenSSL's libcrypto
// elsewhere), fed in parts.
class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  void update(const void* data, size_t size);
  void update(std::string_view s) { update(s.data(), s.size()); }
  std::array<uint8_t, 32> finish();
  std::string finish_hex();

 private:
  struct State;
  std::unique_ptr<State> s_;
};

std::string sha256_hex(std::string_view data);
std::array<uint8_t, 20> sha1(std::string_view data);
std::string base64(const void* data, size_t size);
inline std::string base64(std::string_view s) {
  return base64(s.data(), s.size());
}
// Returns false on malformed input.
bool base64_decode(std::string_view in, std::string& out);
uint32_t crc32(const void* data, size_t size);
std::string hex(const void* data, size_t size);
// Random version 4 UUID from the OS CSPRNG.
std::string uuid4();
// Six lower-case, filename-safe characters; 31^6 possible values. These are
// references, never capabilities. The owner supplies the collision scope.
inline constexpr std::string_view kShortAlphabet =
    "23456789abcdefghjkmnpqrstuvwxyz";
auto short_id() -> std::string;
auto is_short_id(std::string_view value) -> bool;
auto is_run_id(std::string_view value) -> bool;
auto is_operation_id(std::string_view value) -> bool;
// The generator returns a valid short ID.
auto unique_short_id(const std::function<bool(const std::string&)>& exists,
                     const std::function<std::string()>& generate = short_id)
    -> std::string;
std::string random_token(size_t bytes);
bool is_uuid(std::string_view s);
bool is_sha256_hex(std::string_view s);
// Constant time string comparison.
bool equal_secret(std::string_view a, std::string_view b);

}  // namespace tsugumi::crypto
