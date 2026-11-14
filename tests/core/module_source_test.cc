#include <gtest/gtest.h>

#include "module_source.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {
TEST(DisplayModule, WasmEntryIsSelfContainedAndRejectsInvalidOrOversizedBinary) {
  const std::string wasm("\0asm\1\0\0\0\0\xff", 10);
  const auto source = wasm_module_source("export function dispose(){}", wasm);
  EXPECT_NE(source.find(crypto::base64(wasm)), std::string::npos);
  EXPECT_TRUE(source.ends_with("export function dispose(){}"));
  EXPECT_THROW(wasm_module_source("", "not wasm"), std::runtime_error);
  EXPECT_THROW(wasm_module_source(std::string(32u << 20, 'x'), wasm), std::runtime_error);
}
TEST(DisplayModule, ModuleEntryCannotEscapeToHostFiles) {
  const auto root = std::filesystem::temp_directory_path() / ("tsugumi-module-" + crypto::uuid4());
  std::filesystem::create_directories(root);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{root};
  fs::write_atomic(root / "index.js", "export function dispose(){}");
  EXPECT_EQ(module_source(root), "export function dispose(){}");
  std::filesystem::remove(root / "index.js");
  std::filesystem::create_symlink("/etc/passwd", root / "index.js");
  EXPECT_THROW(module_source(root), std::runtime_error);
}

TEST(DisplayModule, PrivateStoreResolvesOnlyCanonicalTemplateAndEmbedsCardWasm) {
  namespace stdfs = std::filesystem;
  const auto root = stdfs::temp_directory_path() / ("tsugumi-module-" + crypto::uuid4());
  struct Cleanup {
    stdfs::path path;
    ~Cleanup() { stdfs::remove_all(path); }
  } cleanup{root};
  const stdfs::path identity("/nix/store/00000000000000000000000000000000-card-view");
  const stdfs::path shared("/nix/store/11111111111111111111111111111111-template/index.js");
  const auto store = root / "nix/store";
  const auto output = store / identity.filename();
  const auto entry = store / shared.parent_path().filename() / "index.js";
  const std::string js("export function mount(){}"), wasm("\0asm\1\0\0\0", 8);
  fs::write_atomic(entry, js);
  fs::write_atomic(output / "view.wasm", wasm);
  stdfs::create_symlink(shared, output / "index.js");
  EXPECT_EQ(module_source(output, store), wasm_module_source(js, wasm));
  stdfs::remove(output / "index.js");
  stdfs::create_symlink(stdfs::path("..") / shared.parent_path().filename() / "index.js", output / "index.js");
  EXPECT_EQ(module_source(output, store), wasm_module_source(js, wasm));
  fs::write_atomic(output / "view.wasm", "invalid");
  EXPECT_THROW(module_source(output, store), std::runtime_error);
  fs::write_atomic(output / "view.wasm", std::string((8u << 20) + 1, 'x'));
  EXPECT_THROW(module_source(output, store), std::runtime_error);
  stdfs::remove(output / "index.js");
  stdfs::create_symlink("/etc/passwd", output / "index.js");
  EXPECT_THROW(module_source(output, store), std::runtime_error);
}

TEST(DisplayModule, SourceCacheIsBoundedImmutableAndRejectsSymlinks) {
  namespace stdfs = std::filesystem;
  const auto cache = stdfs::temp_directory_path() / ("tsugumi-module-cache-" + crypto::uuid4());
  struct Cleanup {
    stdfs::path path;
    ~Cleanup() { stdfs::remove_all(path); }
  } cleanup{cache};
  const std::string identity("/nix/store/00000000000000000000000000000000-card-view");
  EXPECT_FALSE(cached_module_source(cache, identity));
  save_module_source(cache, identity, "export function mount(){}");
  EXPECT_EQ(cached_module_source(cache, identity), "export function mount(){}");
  EXPECT_NO_THROW(save_module_source(cache, identity, "export function mount(){}"));
  EXPECT_THROW(save_module_source(cache, identity, "different"), std::runtime_error);
  EXPECT_THROW(save_module_source(cache, "/etc", ""), std::runtime_error);
  EXPECT_THROW(save_module_source(cache, identity, std::string(kModuleSourceLimit + 1, 'x')), std::runtime_error);
  const auto file = cache / stdfs::path(identity).filename() / "index.js";
  stdfs::resize_file(file, kModuleSourceLimit + 1);
  EXPECT_THROW(cached_module_source(cache, identity), std::runtime_error);
  stdfs::remove(file);
  stdfs::create_symlink("/etc/passwd", file);
  EXPECT_THROW(cached_module_source(cache, identity), std::runtime_error);
}
}  // namespace
}  // namespace tsugumi
