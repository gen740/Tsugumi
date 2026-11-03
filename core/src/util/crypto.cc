#include "util/crypto.hh"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <stdlib.h>  // arc4random_buf
#else
#include <openssl/evp.h>
#include <sys/random.h>
#endif

namespace tsugumi::crypto {

namespace {


inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void random_bytes(void* out, size_t n) {
#if defined(__APPLE__)
  arc4random_buf(out, n);
#else
  auto* p = static_cast<uint8_t*>(out);
  while (n > 0) {
    ssize_t r = getrandom(p, n, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("getrandom failed");
    }
    p += r;
    n -= static_cast<size_t>(r);
  }
#endif
}

}  // namespace

#if defined(__APPLE__)
struct Sha256::State {
  CC_SHA256_CTX ctx;
};

Sha256::Sha256() : s_(std::make_unique<State>()) { CC_SHA256_Init(&s_->ctx); }
Sha256::~Sha256() = default;

void Sha256::update(const void* data, size_t size) {
  // CC_LONG is 32 bit: larger inputs go in parts.
  const auto* p = static_cast<const uint8_t*>(data);
  while (size > 0) {
    auto take = static_cast<CC_LONG>(std::min<size_t>(size, 1u << 30));
    CC_SHA256_Update(&s_->ctx, p, take);
    p += take;
    size -= take;
  }
}

std::array<uint8_t, 32> Sha256::finish() {
  std::array<uint8_t, 32> out;
  CC_SHA256_Final(out.data(), &s_->ctx);
  return out;
}

std::array<uint8_t, 20> sha1(std::string_view data) {
  std::array<uint8_t, 20> out;
  CC_SHA1_CTX ctx;
  CC_SHA1_Init(&ctx);
  CC_SHA1_Update(&ctx, data.data(), static_cast<CC_LONG>(data.size()));
  CC_SHA1_Final(out.data(), &ctx);
  return out;
}
#else
struct Sha256::State {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  ~State() { EVP_MD_CTX_free(ctx); }
};

Sha256::Sha256() : s_(std::make_unique<State>()) {
  if (!s_->ctx || EVP_DigestInit_ex(s_->ctx, EVP_sha256(), nullptr) != 1) throw std::runtime_error("SHA-256 unavailable");
}
Sha256::~Sha256() = default;

void Sha256::update(const void* data, size_t size) { EVP_DigestUpdate(s_->ctx, data, size); }

std::array<uint8_t, 32> Sha256::finish() {
  std::array<uint8_t, 32> out;
  unsigned int n = 0;
  EVP_DigestFinal_ex(s_->ctx, out.data(), &n);
  return out;
}

std::array<uint8_t, 20> sha1(std::string_view data) {
  std::array<uint8_t, 20> out;
  unsigned int n = 0;
  if (EVP_Digest(data.data(), data.size(), out.data(), &n, EVP_sha1(), nullptr) != 1) throw std::runtime_error("SHA-1 unavailable");
  return out;
}
#endif

std::string Sha256::finish_hex() {
  auto d = finish();
  return hex(d.data(), d.size());
}

std::string sha256_hex(std::string_view data) {
  Sha256 h;
  h.update(data);
  return h.finish_hex();
}

std::string base64(const void* data, size_t size) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto* p = static_cast<const uint8_t*>(data);
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    uint32_t v = uint32_t(p[i]) << 16;
    if (i + 1 < size) v |= uint32_t(p[i + 1]) << 8;
    if (i + 2 < size) v |= p[i + 2];
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += i + 1 < size ? tbl[(v >> 6) & 63] : '=';
    out += i + 2 < size ? tbl[v & 63] : '=';
  }
  return out;
}

bool base64_decode(std::string_view in, std::string& out) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  if (in.size() % 4 != 0) return false;
  out.clear();
  out.reserve(in.size() / 4 * 3);
  for (size_t i = 0; i < in.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      char c = in[i + k];
      if (c == '=') {
        if (i + 4 != in.size() || k < 2) return false;
        v[k] = 0;
        ++pad;
      } else {
        if (pad) return false;
        v[k] = val(c);
        if (v[k] < 0) return false;
      }
    }
    uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
    out += static_cast<char>(n >> 16);
    if (pad < 2) out += static_cast<char>((n >> 8) & 0xFF);
    if (pad < 1) out += static_cast<char>(n & 0xFF);
  }
  return true;
}

uint32_t crc32(const void* data, size_t size) {
  static uint32_t table[256] = {};
  static bool init = [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    return true;
  }();
  (void)init;
  uint32_t c = 0xFFFFFFFFu;
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

std::string hex(const void* data, size_t size) {
  static const char* digits = "0123456789abcdef";
  const auto* p = static_cast<const uint8_t*>(data);
  std::string out(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    out[i * 2] = digits[p[i] >> 4];
    out[i * 2 + 1] = digits[p[i] & 15];
  }
  return out;
}

std::string uuid4() {
  uint8_t b[16];
  random_bytes(b, sizeof b);
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  std::string h = hex(b, sizeof b);
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) +
         "-" + h.substr(20, 12);
}

std::string random_token(size_t bytes) {
  std::vector<uint8_t> b(bytes);
  random_bytes(b.data(), b.size());
  return hex(b.data(), b.size());
}

bool is_uuid(std::string_view s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

bool is_sha256_hex(std::string_view s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

bool equal_secret(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

}  // namespace tsugumi::crypto
