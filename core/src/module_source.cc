#include "module_source.hh"

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
constexpr size_t kModuleLimit = 32u << 20;

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
  if (result.size() > kModuleLimit || entry.size() > kModuleLimit - result.size())
    throw std::runtime_error("module entry exceeds 32 MiB");
  result.append(entry);
  return result;
}

auto module_source(const std::filesystem::path& root) -> std::string {
  namespace stdfs = std::filesystem;
  const auto entry = root / "index.js";
  const auto wasm = root / "view.wasm";
  if (stdfs::is_symlink(entry)) {
    // A shared template is another canonical store entry, never an arbitrary
    // file outside the module. The Card's Wasm must remain inside its output.
    const auto target = stdfs::canonical(entry);
    if (target.filename() != "index.js" || !valid_module_identity(target.parent_path().string()) ||
        stdfs::is_symlink(target.parent_path()) || !fs::resolves_inside(root, wasm))
      throw std::runtime_error("invalid shared module entry");
    return wasm_module_source(read_entry(target, kModuleLimit), read_entry(wasm, 8u << 20));
  }
  if (!fs::resolves_inside(root, entry)) throw std::runtime_error("invalid module entry");
  return read_entry(entry, kModuleLimit);
}
}  // namespace tsugumi
