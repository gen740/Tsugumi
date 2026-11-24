#include <gtest/gtest.h>
#include <wasmtime.h>

#include <array>
#include <string>
#include <string_view>

#include "tsugumi_view_host.h"

namespace {

constexpr std::array<std::string_view, 5> kExports{
    R"((memory (export "memory") 1))",
    R"((func (export "tsugumi_view_abi") (result i32) i32.const 0))",
    R"((func (export "tsugumi_view_start") (param i32 i32 i32 i32 i32 f32) (result i32) i32.const 0))",
    R"((func (export "tsugumi_view_resize") (param i32 i32 f32) (result i32) i32.const 0))",
    R"((func (export "tsugumi_view_draw") (param i32) (result i32) i32.const 0))",
};

auto module(std::string_view imports = {},
            std::array<std::string_view, 5> exports = kExports,
            std::string_view extra = {}) -> std::string {
  std::string wat = "(module ";
  wat += imports;
  for (const auto part : exports) wat += part;
  wat += extra;
  wat += ')';
  return wat;
}

auto check(std::string_view wat, std::string_view expected = {}) -> void {
  wasm_byte_vec_t wasm{};
  auto* compile_error = wasmtime_wat2wasm(wat.data(), wat.size(), &wasm);
  if (compile_error) {
    wasm_byte_vec_t message{};
    wasmtime_error_message(compile_error, &message);
    const std::string detail(message.data, message.size);
    wasm_byte_vec_delete(&message);
    wasmtime_error_delete(compile_error);
    FAIL() << detail;
  }
  std::array<char, 1024> error{};
  const auto status =
      tsugumi_view_host_check(reinterpret_cast<const uint8_t*>(wasm.data),
                              wasm.size, error.data(), error.size());
  wasm_byte_vec_delete(&wasm);
  if (expected.empty()) {
    EXPECT_EQ(status, 0) << error.data();
  } else {
    EXPECT_LT(status, 0);
    EXPECT_NE(std::string_view(error.data()).find(expected),
              std::string_view::npos)
        << error.data();
  }
}

TEST(ViewLoad, MinimalModuleWithoutOptionalExports) { check(module()); }

TEST(ViewLoad, OptionalExportsWithMatchingTypes) {
  check(module({}, kExports, R"(
    (func (export "tsugumi_view_message") (param i32) (result i32) i32.const 0)
    (func (export "tsugumi_view_pointer") (param i32 f32 f32 i32 f32 f32 i32) (result i32) i32.const 0)
    (func (export "tsugumi_view_key") (param i32 i32 i32) (result i32) i32.const 0))"));
}

TEST(ViewLoad, RejectsForeignModules) {
  check(
      module(
          R"((import "env" "tsugumi_view_send" (func (param i32 i32) (result i32))))"),
      "env");
  check(
      module(
          R"((import "wasi_snapshot_preview1" "proc_exit" (func (param i32))))"),
      "wasi_snapshot_preview1");
}

TEST(ViewLoad, RejectsUnknownWebGpuName) {
  check(module(R"((import "webgpu" "wgpuDeviceRelease" (func (param i32))))"),
        "wgpuDeviceRelease");
}

TEST(ViewLoad, AllowsListedWebGpuImport) {
  check(module(
      R"((import "webgpu" "wgpuDeviceCreateBuffer" (func (param i32 i32) (result i32))))"));
}

TEST(ViewLoad, WebGpuCallDuringAbiTrapsWithoutADevice) {
  auto exports = kExports;
  exports[1] = R"((func (export "tsugumi_view_abi") (result i32)
    i32.const 0 i32.const 0 call $createBuffer))";
  check(module(R"((import "webgpu" "wgpuDeviceCreateBuffer"
      (func $createBuffer (param i32 i32) (result i32))))",
               exports),
        "wgpuDeviceCreateBuffer");
}

TEST(ViewLoad, RejectsMemoryImportEvenWithAllowedName) {
  check(module(R"((import "tsugumi" "tsugumi_view_read" (memory 1)))"),
        "tsugumi_view_read");
}

TEST(ViewLoad, RejectsWrongImportSignature) {
  check(module(R"((import "tsugumi" "tsugumi_view_send" (func (param i32))))"),
        "tsugumi_view_send");
}

TEST(ViewLoad, RejectsEachMissingRequiredExport) {
  constexpr std::array names{"memory", "tsugumi_view_abi", "tsugumi_view_start",
                             "tsugumi_view_resize", "tsugumi_view_draw"};
  for (size_t i = 0; i < kExports.size(); ++i) {
    SCOPED_TRACE(names[i]);
    auto exports = kExports;
    exports[i] = "";
    check(module({}, exports), names[i]);
  }
}

TEST(ViewLoad, RejectsWrongExportKindAndSignatures) {
  auto exports = kExports;
  exports[0] = R"((global (export "memory") i32 (i32.const 0)))";
  check(module({}, exports), "memory");
  exports = kExports;
  exports[4] =
      R"((func (export "tsugumi_view_draw") (result i32) i32.const 0))";
  check(module({}, exports), "tsugumi_view_draw");
  check(module({}, kExports, R"((func (export "tsugumi_view_key")) )"),
        "tsugumi_view_key");
}

TEST(ViewLoad, RejectsUnsupportedAbi) {
  auto exports = kExports;
  exports[1] = R"((func (export "tsugumi_view_abi") (result i32) i32.const 1))";
  check(module({}, exports), "Unsupported view ABI 1");
}

TEST(ViewLoad, RejectsTrappingAbi) {
  auto exports = kExports;
  exports[1] = R"((func (export "tsugumi_view_abi") (result i32) unreachable))";
  check(module({}, exports), "tsugumi_view_abi");
}

TEST(ViewLoad, CanCallMessageImportsFromAbiWithoutReadingOrSending) {
  auto exports = kExports;
  exports[1] = R"((func (export "tsugumi_view_abi") (result i32)
    i32.const 65536 i32.const 1 call $read i32.const -1 i32.ne
    i32.const 0 i32.const 0 call $read i32.const -1 i32.ne i32.or
    i32.const 0 i32.const 0 i32.const 0 call $send i32.const -1 i32.ne i32.or))";
  check(module(R"(
    (import "tsugumi" "tsugumi_view_read" (func $read (param i32 i32) (result i32)))
    (import "tsugumi" "tsugumi_view_send" (func $send (param i32 i32 i32) (result i32))))",
               exports));
}

}  // namespace
