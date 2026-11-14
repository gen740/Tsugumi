#include "module_source.hh"

#include <mutex>
#include <stdexcept>

#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
auto valid_module_identity(std::string_view identity) -> bool {
  if (!identity.starts_with("/nix/store/") || identity.size() < 45 || identity[43] != '-') return false;
  constexpr std::string_view hash = "0123456789abcdfghijklmnpqrsvwxyz";
  for (size_t i = 11; i < 43; ++i)
    if (hash.find(identity[i]) == std::string_view::npos) return false;
  constexpr std::string_view name = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-._?=";
  return identity.substr(44).find_first_not_of(name) == std::string_view::npos;
}

namespace {
auto read_entry(const std::filesystem::path& entry, size_t limit) -> std::string {
  if (!std::filesystem::is_regular_file(entry)) throw std::runtime_error("module entry is missing");
  if (std::filesystem::file_size(entry) > limit) throw std::runtime_error("module entry exceeds size limit");
  return fs::read_file_or_throw(entry);
}
}  // namespace

auto wasm_module_source(std::string_view entry, std::string_view wasm) -> std::string {
  if (wasm.size() < 8 || wasm.substr(0, 8) != std::string_view("\0asm\1\0\0\0", 8))
    throw std::runtime_error("invalid view Wasm");
  // No fetch or platform-specific loader in either UI. Only immutable build
  // outputs are assembled here; no per-run data is cached or substituted.
  auto result = "const tsugumiWasm=Uint8Array.from(atob(\"" + crypto::base64(wasm) + "\"),c=>c.charCodeAt(0));\n";
  if (result.size() > kModuleSourceLimit || entry.size() > kModuleSourceLimit - result.size())
    throw std::runtime_error("module entry exceeds 32 MiB");
  result.append(entry);
  return result;
}

auto module_source(const std::filesystem::path& root, const std::filesystem::path& store_directory) -> std::string {
  namespace stdfs = std::filesystem;
  const auto entry = root / "index.js";
  const auto wasm = root / "view.wasm";
  if (stdfs::is_symlink(entry)) {
    // A shared template is another canonical store entry, never an arbitrary
    // file outside the module. The Card's Wasm must remain inside its output.
    const auto link = stdfs::read_symlink(entry);
    // Absolute links keep their canonical identity even in a private store;
    // relative links must resolve to a sibling output in that same store.
    const auto target = link.is_absolute() ? store_directory / link.parent_path().filename() / link.filename() :
                                           (root / link).lexically_normal();
    const auto identity = link.is_absolute() ? link : stdfs::path("/nix/store") / target.parent_path().filename() / target.filename();
    if (identity.filename() != "index.js" || !valid_module_identity(identity.parent_path().string()) ||
        target.parent_path().parent_path() != store_directory)
      throw std::runtime_error("invalid shared module entry");
    if (stdfs::is_symlink(target.parent_path()) || stdfs::is_symlink(target) || !fs::resolves_inside(root, wasm))
      throw std::runtime_error("invalid shared module entry");
    return wasm_module_source(read_entry(target, kModuleSourceLimit), read_entry(wasm, 8u << 20));
  }
  if (!fs::resolves_inside(root, entry)) throw std::runtime_error("invalid module entry");
  return read_entry(entry, kModuleSourceLimit);
}

auto cached_module_source(const std::filesystem::path& cache, std::string_view identity) -> std::optional<std::string> {
  if (!valid_module_identity(identity)) throw std::runtime_error("invalid module identity");
  const auto rel = std::filesystem::path(identity).filename() / "index.js";
  if (!std::filesystem::exists(cache / rel)) return std::nullopt;
  std::string source;
  fs::stream_beneath(cache, rel, kModuleSourceLimit, [&](std::string_view chunk) { source.append(chunk); });
  return source;
}

auto save_module_source(const std::filesystem::path& cache, std::string_view identity, std::string_view source) -> void {
  if (!valid_module_identity(identity)) throw std::runtime_error("invalid module identity");
  if (source.size() > kModuleSourceLimit) throw std::runtime_error("module entry exceeds 32 MiB");
  // Concurrent imports may share an immutable module, never replace its bytes.
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  if (auto saved = cached_module_source(cache, identity)) {
    if (*saved != source) throw std::runtime_error("conflicting module source for identity");
    return;
  }
  fs::write_atomic(cache / std::filesystem::path(identity).filename() / "index.js", source, 0600);
}
}  // namespace tsugumi
