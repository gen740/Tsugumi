#include "util/crypto.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

#if defined(__APPLE__)
#include <stdlib.h>  // arc4random_buf
#else
#include <sys/random.h>
#endif

namespace tsugumi::crypto {

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
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

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
         0x5be0cd19} {}

void Sha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
           (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6],
           h = h_[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + kK[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
  h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  total_ += size;
  while (size > 0) {
    size_t take = std::min(size, sizeof buf_ - buf_len_);
    std::memcpy(buf_ + buf_len_, p, take);
    buf_len_ += take;
    p += take;
    size -= take;
    if (buf_len_ == sizeof buf_) {
      block(buf_);
      buf_len_ = 0;
    }
  }
}

std::array<uint8_t, 32> Sha256::finish() {
  uint64_t bits = total_ * 8;
  uint8_t pad = 0x80;
  update(&pad, 1);
  uint8_t zero = 0;
  while (buf_len_ != 56) update(&zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  update(len, 8);
  std::array<uint8_t, 32> out;
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<uint8_t>(h_[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
  }
  return out;
}

std::string Sha256::finish_hex() {
  auto d = finish();
  return hex(d.data(), d.size());
}

std::string sha256_hex(std::string_view data) {
  Sha256 h;
  h.update(data);
  return h.finish_hex();
}

std::array<uint8_t, 20> sha1(std::string_view data) {
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
  std::string msg(data);
  uint64_t bits = uint64_t(data.size()) * 8;
  msg += static_cast<char>(0x80);
  while (msg.size() % 64 != 56) msg += '\0';
  for (int i = 7; i >= 0; --i) msg += static_cast<char>(bits >> (i * 8));
  for (size_t off = 0; off < msg.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const uint8_t*>(msg.data() + off + i * 4);
      w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
  }
  std::array<uint8_t, 20> out;
  uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(hs[i] >> (24 - 8 * j));
  return out;
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
