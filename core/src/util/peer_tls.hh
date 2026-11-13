#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace tsugumi::peer_tls {
// Synchronous framed I/O requires blocking accepted sockets on every OS.
// Also configures deadlines, close-on-exec and SO_NOSIGPIPE where available.
auto ConfigureSocket(int fd, int timeout_seconds) -> void;

// Canonical Ed25519 SubjectPublicKeyInfo, base64 encoded after "ed25519:".
auto ValidateKey(std::string_view value) -> void;
auto Authorized(const std::filesystem::path& file, std::string_view key)
    -> bool;

class Identity {
 public:
  explicit Identity(const std::filesystem::path& file);
  ~Identity();
  Identity(const Identity&) = delete;
  auto operator=(const Identity&) -> Identity& = delete;
  auto public_key() const -> std::string;

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
  // The caller owns fd and configures its socket deadlines before handshake.
  Stream(const Context& context, int fd, bool server);
  ~Stream();
  Stream(const Stream&) = delete;
  auto operator=(const Stream&) -> Stream& = delete;
  auto Read(size_t count) -> std::string;
  auto Write(std::string_view bytes) -> void;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi::peer_tls
