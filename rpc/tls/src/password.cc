#include "tsugumi/rpc/password.hh"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace tsugumi::password {
namespace {
constexpr uint64_t kLogN = 15, kR = 8, kP = 1;
constexpr size_t kSalt = 16, kKey = 32;
// Enough for N = 2^15, r = 8 (32 MiB) with headroom; a stored hash asking
// for more is refused rather than allowed to exhaust memory.
constexpr uint64_t kMaxMemory = uint64_t{64} << 20;

auto Hex(const std::vector<unsigned char>& bytes) -> std::string {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (const auto byte : bytes) {
    text += kDigits[byte >> 4];
    text += kDigits[byte & 15];
  }
  return text;
}
auto Unhex(std::string_view text, std::vector<unsigned char>& bytes) -> bool {
  if (text.size() % 2) return false;
  bytes.clear();
  for (size_t i = 0; i < text.size(); i += 2) {
    unsigned value = 0;
    const auto [end, error] =
        std::from_chars(text.data() + i, text.data() + i + 2, value, 16);
    if (error != std::errc{} || end != text.data() + i + 2) return false;
    bytes.push_back(static_cast<unsigned char>(value));
  }
  return true;
}
auto Derive(std::string_view password, const std::vector<unsigned char>& salt,
            uint64_t log_n, uint64_t r, uint64_t p, size_t size,
            std::vector<unsigned char>& key) -> bool {
  key.assign(size, 0);
  return EVP_PBE_scrypt(password.data(), password.size(), salt.data(),
                        salt.size(), uint64_t{1} << log_n, r, p, kMaxMemory,
                        key.data(), key.size()) == 1;
}
auto Number(std::string_view text, uint64_t& value) -> bool {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}
}  // namespace

auto Hash(std::string_view password) -> std::string {
  std::vector<unsigned char> salt(kSalt), key;
  if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
      !Derive(password, salt, kLogN, kR, kP, kKey, key))
    throw std::runtime_error("Cannot hash the password");
  return "scrypt$" + std::to_string(kLogN) + "$" + std::to_string(kR) + "$" +
         std::to_string(kP) + "$" + Hex(salt) + "$" + Hex(key);
}

auto Verify(std::string_view stored, std::string_view password) -> bool {
  std::vector<std::string_view> parts;
  for (size_t begin = 0;;) {
    const auto end = stored.find('$', begin);
    parts.push_back(stored.substr(begin, end - begin));
    if (end == stored.npos) break;
    begin = end + 1;
  }
  uint64_t log_n = 0, r = 0, p = 0;
  std::vector<unsigned char> salt, expected, key;
  if (parts.size() != 6 || parts[0] != "scrypt" || !Number(parts[1], log_n) ||
      !Number(parts[2], r) || !Number(parts[3], p) || log_n < 10 ||
      log_n > 20 || r < 1 || r > 16 || p < 1 || p > 4 ||
      !Unhex(parts[4], salt) || salt.size() < 16 ||
      !Unhex(parts[5], expected) || expected.size() < 16 ||
      !Derive(password, salt, log_n, r, p, expected.size(), key))
    return false;
  return CRYPTO_memcmp(key.data(), expected.data(), key.size()) == 0;
}

}  // namespace tsugumi::password
