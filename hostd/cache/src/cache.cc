#include "tsugumi/cache/cache.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <nix/store/nar-info.hh>
#include <nix/store/store-dir-config.hh>
#include <nix/util/compression.hh>
#include <nix/util/hash.hh>
#include <nix/util/signature/local-keys.hh>
#include <nix/util/signature/signer.hh>
#include <utility>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi::cache {
namespace stdfs = std::filesystem;
namespace {

constexpr std::string_view kNix32 = "0123456789abcdfghijklmnpqrsvwxyz";
constexpr size_t kNarInfoLimit = 1 << 20;
constexpr size_t kListingLimit = 64 << 20;

auto IsNix32(std::string_view text, size_t size) -> bool {
  return text.size() == size && text.find_first_not_of(kNix32) == text.npos;
}

enum class Kind { kCacheInfo, kNarInfo, kListing, kNar };
struct Name {
  Kind kind;
  std::string hash;
};
// Only the names Nix's binary cache stores write. Every other name, including
// anything with "/", ".." or "%" outside these shapes, is not part of the
// cache.
auto ParseName(std::string_view name) -> std::optional<Name> {
  if (name == "nix-cache-info") return Name{Kind::kCacheInfo, {}};
  for (const auto& [suffix, kind] :
       {std::pair{std::string_view(".narinfo"), Kind::kNarInfo},
        std::pair{std::string_view(".ls"), Kind::kListing}}) {
    if (name.ends_with(suffix)) {
      const auto stem = name.substr(0, name.size() - suffix.size());
      if (IsNix32(stem, 32)) return Name{kind, std::string(stem)};
      return std::nullopt;
    }
  }
  if (!name.starts_with("nar/")) return std::nullopt;
  name.remove_prefix(4);
  const auto dot = name.find(".nar");
  if (dot == name.npos || !IsNix32(name.substr(0, dot), 52))
    return std::nullopt;
  const auto extension = name.substr(dot + 4);
  for (const std::string_view known :
       {"", ".xz", ".bz2", ".zst", ".lzip", ".lz4", ".br"})
    if (extension == known) return Name{Kind::kNar, std::string(name, 0, dot)};
  return std::nullopt;
}

auto ContentType(Kind kind) -> std::string {
  switch (kind) {
    case Kind::kCacheInfo:
      return "text/x-nix-cache-info";
    case Kind::kNarInfo:
      return "text/x-nix-narinfo";
    case Kind::kListing:
      return "application/json";
    case Kind::kNar:
      return "application/x-nix-nar";
  }
  return "application/octet-stream";
}

auto Fail(const std::string& what) -> void {
  throw std::runtime_error(what + ": " + std::strerror(errno));
}

// A file in the uploads folder. It reaches its served name only through
// Place, which links it without replacing anything; the staging name itself is
// always removed, so an upload that never commits leaves nothing behind
// unless the process dies mid-upload.
class Staged {
 public:
  explicit Staged(const stdfs::path& uploads)
      : file_(uploads / ("upload-" + crypto::random_token(12))) {
    fd_ = ::open(file_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd_ < 0) Fail("create " + file_.string());
  }
  ~Staged() {
    if (fd_ >= 0) ::close(fd_);
    ::unlink(file_.c_str());
  }
  Staged(const Staged&) = delete;
  auto operator=(const Staged&) -> Staged& = delete;

  auto Write(std::string_view bytes) -> void {
    while (!bytes.empty()) {
      const auto n = ::write(fd_, bytes.data(), bytes.size());
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) Fail("write " + file_.string());
      bytes.remove_prefix(static_cast<size_t>(n));
    }
  }
  // False when the name already exists: its first content stays.
  auto Place(const stdfs::path& target) -> bool {
    if (::fsync(fd_) != 0) Fail("fsync " + file_.string());
    ::close(fd_);
    fd_ = -1;
    if (::link(file_.c_str(), target.c_str()) != 0) {
      if (errno == EEXIST) return false;
      Fail("link " + target.string());
    }
    fs::fsync_dir(target.parent_path());
    return true;
  }

 private:
  stdfs::path file_;
  int fd_ = -1;
};

auto ReadChunks(const stdfs::path& path,
                const std::function<void(std::string_view)>& consume) -> void {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) Fail("open " + path.string());
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } close{fd};
  std::array<char, 1 << 16> buffer;
  while (true) {
    const auto n = ::read(fd, buffer.data(), buffer.size());
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) Fail("read " + path.string());
    if (n == 0) return;
    consume(std::string_view(buffer.data(), static_cast<size_t>(n)));
  }
}

auto RegularSize(const stdfs::path& path) -> std::optional<uint64_t> {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    return std::nullopt;
  return static_cast<uint64_t>(st.st_size);
}

}  // namespace

struct Cache::State {
  stdfs::path root, uploads;
  std::string store_dir;
  nix::StoreDirConfig store{store_dir};
  nix::LocalSigner signer;

  State(stdfs::path root, stdfs::path uploads, const std::string& secret_key,
        std::string store_dir)
      : root(std::move(root)),
        uploads(std::move(uploads)),
        store_dir(std::move(store_dir)),
        signer(nix::SecretKey(secret_key)) {}

  // The uploaded narinfo, checked against what the cache holds, and signed.
  auto Verify(const std::string& text, const std::string& hash) -> std::string {
    try {
      nix::NarInfo info(store, text, "uploaded narinfo");
      if (info.path.hashPart() != hash)
        throw Rejected(400, "The narinfo describes another store path");
      const auto nar = ParseName(info.url);
      if (!nar || nar->kind != Kind::kNar)
        throw Rejected(400, "The narinfo's URL is not a NAR file name");
      const auto file = root / info.url;
      const auto size = RegularSize(file);
      if (!size) throw Rejected(409, "Upload the NAR before its narinfo");
      if (!info.fileHash || info.fileHash->algo != nix::HashAlgorithm::SHA256 ||
          info.fileHash->to_string(nix::HashFormat::Nix32, false) !=
              nar->hash ||
          (info.fileSize && info.fileSize != *size))
        throw Rejected(400, "The narinfo's file hash or size is not its NAR's");
      nix::HashSink hasher(info.narHash.algo);
      auto decompress = nix::makeDecompressionSink(
          info.compression.empty() ? "none" : info.compression, hasher);
      ReadChunks(file, [&](std::string_view bytes) { (*decompress)(bytes); });
      decompress->finish();
      const auto [nar_hash, nar_size] = hasher.finish();
      if (nar_hash != info.narHash || nar_size != info.narSize)
        throw Rejected(400, "The NAR does not have the narinfo's NAR hash");
      for (const auto& reference : info.references)
        if (reference != info.path &&
            !RegularSize(root /
                         (std::string(reference.hashPart()) + ".narinfo")))
          throw Rejected(409, "Reference " + store.printStorePath(reference) +
                                  " is not in the cache yet");
      info.sigs.clear();
      info.sigs.insert(signer.signDetached(info.fingerprint(store)));
      return info.to_string(store);
    } catch (const nix::Error& error) {
      throw Rejected(400, error.what());
    }
  }
};

namespace {

class NarUpload : public Cache::Upload {
 public:
  NarUpload(const stdfs::path& uploads, stdfs::path target, std::string hash)
      : staged_(uploads), target_(std::move(target)), hash_(std::move(hash)) {}
  auto Write(std::string_view bytes) -> void override {
    staged_.Write(bytes);
    hasher_(bytes);
  }
  auto Commit() -> void override {
    if (hasher_.finish().hash.to_string(nix::HashFormat::Nix32, false) != hash_)
      throw Rejected(400, "The NAR file does not have the hash in its name");
    staged_.Place(target_);
  }

 private:
  Staged staged_;
  stdfs::path target_;
  std::string hash_;
  nix::HashSink hasher_{nix::HashAlgorithm::SHA256};
};

// Small files are held in memory until the request ends.
class TextUpload : public Cache::Upload {
 public:
  using Finish = std::function<std::string(const std::string&)>;
  TextUpload(stdfs::path uploads, stdfs::path target, std::string encoding,
             size_t limit, Finish finish)
      : uploads_(std::move(uploads)),
        target_(std::move(target)),
        encoding_(std::move(encoding)),
        limit_(limit),
        finish_(std::move(finish)) {}
  auto Write(std::string_view bytes) -> void override {
    if (body_.size() + bytes.size() > limit_)
      throw Rejected(413, "The file is too large");
    body_.append(bytes);
  }
  auto Commit() -> void override {
    if (RegularSize(target_)) return;
    std::string text;
    try {
      text = encoding_.empty() ? body_ : nix::decompress(encoding_, body_);
    } catch (const nix::Error& error) {
      throw Rejected(400, error.what());
    }
    if (text.size() > limit_) throw Rejected(413, "The file is too large");
    Staged staged(uploads_);
    staged.Write(finish_(text));
    staged.Place(target_);
  }

 private:
  stdfs::path uploads_, target_;
  std::string encoding_;
  size_t limit_;
  Finish finish_;
  std::string body_;
};

}  // namespace

Cache::Cache(stdfs::path root, stdfs::path uploads,
             const std::string& secret_key, std::string store_dir)
    : state_(std::make_unique<State>(std::move(root), std::move(uploads),
                                     secret_key, std::move(store_dir))) {
  stdfs::create_directories(state_->root / "nar");
  stdfs::create_directories(state_->uploads);
  const auto info = state_->root / "nix-cache-info";
  if (!RegularSize(info)) {
    Staged staged(state_->uploads);
    staged.Write("StoreDir: " + state_->store_dir +
                 "\nWantMassQuery: 1\nPriority: 30\n");
    staged.Place(info);
  }
}
Cache::~Cache() = default;

auto Cache::public_key() const -> std::string {
  return state_->signer.getPublicKey().to_string();
}

auto Cache::Find(std::string_view name) const -> std::optional<File> {
  const auto parsed = ParseName(name);
  if (!parsed) return std::nullopt;
  auto path = state_->root / name;
  if (!RegularSize(path)) return std::nullopt;
  return File{std::move(path), ContentType(parsed->kind)};
}

auto Cache::Begin(std::string_view name, std::string_view encoding)
    -> std::unique_ptr<Upload> {
  const auto parsed = ParseName(name);
  if (!parsed) throw Rejected(404, "Not a binary cache file name");
  const auto target = state_->root / name;
  switch (parsed->kind) {
    case Kind::kCacheInfo:
      throw Rejected(403, "nix-cache-info belongs to the server");
    case Kind::kNar:
      if (!encoding.empty())
        throw Rejected(415, "NAR files are compressed by their name");
      return std::make_unique<NarUpload>(state_->uploads, target, parsed->hash);
    case Kind::kNarInfo:
      return std::make_unique<TextUpload>(
          state_->uploads, target, std::string(encoding), kNarInfoLimit,
          [this, hash = parsed->hash](const std::string& text) {
            return state_->Verify(text, hash);
          });
    case Kind::kListing:
      return std::make_unique<TextUpload>(
          state_->uploads, target, std::string(encoding), kListingLimit,
          [](const std::string& text) { return text; });
  }
  throw Rejected(404, "Not a binary cache file name");
}

auto Measure(const stdfs::path& root, const stdfs::path& uploads) -> Usage {
  Usage usage;
  std::error_code error;
  for (stdfs::recursive_directory_iterator it(root, error), end;
       !error && it != end; it.increment(error)) {
    if (!it->is_regular_file(error)) continue;
    usage.bytes += it->file_size(error);
    if (it->path().parent_path() == root &&
        it->path().extension() == ".narinfo")
      ++usage.paths;
    else if (it->path().parent_path() == root / "nar")
      ++usage.nars;
  }
  for (stdfs::directory_iterator it(uploads, error), end; !error && it != end;
       it.increment(error)) {
    if (!it->is_regular_file(error)) continue;
    ++usage.incomplete_files;
    usage.incomplete_bytes += it->file_size(error);
  }
  return usage;
}

auto LoadOrCreateSecretKey(const stdfs::path& file) -> std::string {
  if (auto text = fs::read_file(file)) {
    while (!text->empty() && (text->back() == '\n' || text->back() == '\r'))
      text->pop_back();
    nix::SecretKey key(*text);  // validates
    return *text;
  }
  const auto key =
      nix::SecretKey::generate("tsugumi-cache-" + crypto::random_token(6))
          .to_string();
  fs::write_atomic(file, key + "\n", 0600);
  return key;
}

auto PublicKeyOf(const std::string& secret_key) -> std::string {
  return nix::SecretKey(secret_key).toPublicKey().to_string();
}

}  // namespace tsugumi::cache
