// Hashing, encoding and identifier helpers.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace tsugumi::crypto {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t size);
  void update(std::string_view s) { update(s.data(), s.size()); }
  std::array<uint8_t, 32> finish();
  std::string finish_hex();

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t buf_len_ = 0;
  uint64_t total_ = 0;
};

std::string sha256_hex(std::string_view data);
std::array<uint8_t, 20> sha1(std::string_view data);
std::string base64(const void* data, size_t size);
inline std::string base64(std::string_view s) { return base64(s.data(), s.size()); }
// Returns false on malformed input.
bool base64_decode(std::string_view in, std::string& out);
uint32_t crc32(const void* data, size_t size);
std::string hex(const void* data, size_t size);
// Random version 4 UUID from the OS CSPRNG.
std::string uuid4();
std::string random_token(size_t bytes);
bool is_uuid(std::string_view s);
bool is_sha256_hex(std::string_view s);
// Constant time string comparison.
bool equal_secret(std::string_view a, std::string_view b);

}  // namespace tsugumi::crypto
