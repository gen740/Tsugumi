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
}  // namespace
}  // namespace tsugumi
