#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tsugumi::peer_tls {
// Synchronous framed I/O requires blocking accepted sockets on every OS.
// Also configures close-on-exec and SO_NOSIGPIPE where available.
auto ConfigureSocket(int fd, int timeout_seconds = 0) -> void;

// Canonical Ed25519 SubjectPublicKeyInfo, base64 encoded after "ed25519:".
auto ValidateKey(std::string_view value) -> void;
// Whether an authorized_keys file lists `key`: one public key per line, blank
// lines and `#` comments allowed. The file must be an owned regular file not
// writable by others; a malformed file authorizes nothing. Callers re-read it
// at every handshake so removing a line revokes new connections.
auto Authorized(const std::filesystem::path& file, std::string_view key)
    -> bool;

class Identity {
 public:
  explicit Identity(const std::filesystem::path& file);
  // A server reached by clients that verify host names (curl, and so Nix)
  // presents a certificate for `names` (DNS names or IP addresses) instead
  // of the stored one. It is self-signed by the same key and the same bytes
  // on every start, so a client can keep trusting the file it was given.
  Identity(const std::filesystem::path& file,
           const std::vector<std::string>& names);
  ~Identity();
  Identity(const Identity&) = delete;
  auto operator=(const Identity&) -> Identity& = delete;
  auto public_key() const -> std::string;
  auto certificate_pem() const -> std::string;

 private:
  struct State;
  std::unique_ptr<State> state_;
  friend class Context;
};

// Replaces CA/hostname verification with an explicit SPKI decision, on both
// sides. No session resumption or early data may bypass a current key check.
class Context {
 public:
  Context(const Identity& identity, bool server,
          std::function<bool(std::string_view)> accepts);
  ~Context();
  Context(const Context&) = delete;
  auto operator=(const Context&) -> Context& = delete;

 private:
  struct State;
  std::unique_ptr<State> state_;
  friend class Stream;
};
class Stream {
 public:
  // The caller owns fd. If a downstream fd is supplied, fd is nonblocking
  // and a pending TLS operation also wakes when the downstream disconnects.
  Stream(const Context& context, int fd, bool server, int downstream = -1);
  ~Stream();
  Stream(const Stream&) = delete;
  auto operator=(const Stream&) -> Stream& = delete;
  auto Read(size_t count) -> std::string;
  // Up to `limit` bytes as soon as any arrive; empty when the peer closed.
  auto ReadSome(size_t limit) -> std::string;
  // The peer's authenticated public key, in the form `accepts` received.
  auto peer_key() const -> std::string;
  auto Write(std::string_view bytes) -> void;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi::peer_tls
