// Tsugumi's own Nix binary cache: one implementation behind the pull list
// and the push target (docs/design/BinaryCacheServer.md).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tsugumi::cache {

// A request the cache refuses, with the HTTP status that says why.
class Rejected : public std::runtime_error {
 public:
  Rejected(int status, const std::string& message)
      : std::runtime_error(message), status(status) {}
  int status;
};

// What the cache folder holds. Nothing is removed automatically, so this is
// what a person sees before deciding to delete.
struct Usage {
  uint64_t bytes = 0;  // every file in the binary cache folder
  uint64_t paths = 0;  // store paths (.narinfo files)
  uint64_t nars = 0;   // compressed NAR files
  // Files of uploads that never finished (the server stopped mid-upload).
  // They are never served, and kept for a person to remove.
  uint64_t incomplete_files = 0;
  uint64_t incomplete_bytes = 0;
};
auto Measure(const std::filesystem::path& root,
             const std::filesystem::path& uploads) -> Usage;

// A folder in the standard file:// binary-cache layout (`nix-cache-info`,
// `<hash>.narinfo`, `nar/<file hash>.nar.<ext>`), so its contents are plain
// Nix files. Uploads are staged in `uploads` (same filesystem) and appear
// under their name with one link(2): a name is served complete or not at all.
//
// A .narinfo is accepted only when its NAR is present with the stated file
// hash and size, the decompressed NAR is one valid NAR with the stated NAR
// hash and size, and every reference other than itself is already present
// under its full store path. The cache then signs
// it with its own key, replacing the uploader's signatures. A name that
// exists keeps its first content; nothing is replaced or removed.
class Cache {
 public:
  // `secret_key` is a Nix secret key (`name:base64`).
  Cache(std::filesystem::path root, std::filesystem::path uploads,
        const std::string& secret_key, std::string store_dir = "/nix/store");
  ~Cache();
  Cache(const Cache&) = delete;
  auto operator=(const Cache&) -> Cache& = delete;

  // The key clients put in `trusted-public-keys`.
  auto public_key() const -> std::string;

  struct File {
    std::filesystem::path path;
    std::string content_type;
  };
  // A served name (the URL path without its leading slash), when present.
  auto Find(std::string_view name) const -> std::optional<File>;

  class Upload {
   public:
    virtual ~Upload() = default;  // without Commit: nothing appears
    virtual auto Write(std::string_view bytes) -> void = 0;
    virtual auto Commit() -> void = 0;
  };
  // Throws Rejected for names that cannot be uploaded. `encoding` is the
  // request's Content-Encoding (Nix's narinfo-compression), or empty.
  auto Begin(std::string_view name, std::string_view encoding)
      -> std::unique_ptr<Upload>;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

// Reads and generates the cache's Nix signing key. A new key gets a random
// name so that two hosts' caches never share a `trusted-public-keys` name.
auto LoadOrCreateSecretKey(const std::filesystem::path& file) -> std::string;
auto PublicKeyOf(const std::string& secret_key) -> std::string;

}  // namespace tsugumi::cache
