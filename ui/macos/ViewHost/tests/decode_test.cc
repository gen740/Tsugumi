#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "webgpu_imports.gen.h"

namespace tsugumi::view {
namespace {

template <class T>
auto native() -> T {
  static int token;
  return reinterpret_cast<T>(&token);
}

class DecodeTest : public testing::Test {
 protected:
  template <class T = uint32_t>
  auto put(uint32_t address, T value) -> void {
    std::memcpy(bytes_.data() + address, &value, sizeof(T));
  }

  auto putText(uint32_t address, std::string_view text) -> void {
    std::memcpy(bytes_.data() + address, text.data(), text.size());
  }

  auto invoke(std::string_view name, std::initializer_list<Val> args)
      -> uint32_t {
    const auto entries = imports();
    const auto iter = std::ranges::find(entries, name, &Import::name);
    if (iter == entries.end() || iter->params.size() != args.size()) {
      throw std::logic_error("invalid import in test");
    }
    Val result{};
    iter->call(context_, args.begin(), &result);
    return std::bit_cast<uint32_t>(result.i32);
  }

  auto word(uint32_t value) -> Val {
    return Val{.i32 = std::bit_cast<int32_t>(value)};
  }

  auto expectTrap(std::string_view name, std::initializer_list<Val> args,
                  std::string_view field, std::string_view reason) -> void {
    try {
      invoke(name, args);
      FAIL() << "import accepted invalid data";
    } catch (const TrapError& error) {
      const std::string_view message(error.what());
      EXPECT_TRUE(message.find(name) != std::string_view::npos) << message;
      EXPECT_TRUE(message.find(field) != std::string_view::npos) << message;
      EXPECT_TRUE(message.find(reason) != std::string_view::npos) << message;
    }
  }

  std::vector<uint8_t> bytes_ = std::vector<uint8_t>(1024);
  Handles handles_;
  Procs procs_;
  Context context_{{bytes_.data(), bytes_.size()}, handles_, procs_};
};

auto checkRenderPipeline(const WGPURenderPipelineDescriptor& descriptor)
    -> void {
  EXPECT_EQ(descriptor.layout, nullptr);
  const auto& vertex = descriptor.vertex;
  EXPECT_EQ(vertex.module, native<WGPUShaderModule>());
  EXPECT_EQ(std::string_view(vertex.entryPoint.data, vertex.entryPoint.length),
            "vs");
  ASSERT_EQ(vertex.bufferCount, 1);
  ASSERT_NE(vertex.buffers, nullptr);
  const auto& buffer = vertex.buffers[0];
  EXPECT_EQ(buffer.stepMode, WGPUVertexStepMode_Instance);
  EXPECT_EQ(buffer.arrayStride, 16);
  ASSERT_EQ(buffer.attributeCount, 1);
  ASSERT_NE(buffer.attributes, nullptr);
  EXPECT_EQ(buffer.attributes[0].format, WGPUVertexFormat_Float32x3);
  EXPECT_EQ(buffer.attributes[0].offset, 4);
  EXPECT_EQ(buffer.attributes[0].shaderLocation, 2);
  ASSERT_NE(descriptor.fragment, nullptr);
  EXPECT_EQ(descriptor.fragment->module, vertex.module);
  EXPECT_EQ(descriptor.fragment->entryPoint.data, nullptr);
  EXPECT_EQ(descriptor.fragment->entryPoint.length, WGPU_STRLEN);
  ASSERT_EQ(descriptor.fragment->targetCount, 1);
  ASSERT_NE(descriptor.fragment->targets, nullptr);
  const auto& target = descriptor.fragment->targets[0];
  EXPECT_EQ(target.format, WGPUTextureFormat_BGRA8Unorm);
  EXPECT_EQ(target.writeMask, WGPUColorWriteMask_All);
  ASSERT_NE(descriptor.depthStencil, nullptr);
  EXPECT_EQ(descriptor.depthStencil->format, WGPUTextureFormat_Depth24Plus);
  EXPECT_EQ(descriptor.depthStencil->depthCompare, WGPUCompareFunction_Less);
  EXPECT_EQ(descriptor.depthStencil->depthWriteEnabled, WGPUOptionalBool_True);
}

TEST_F(DecodeTest, RenderPipeline) {
  const auto device = handles_.add(Kind::Device, native<WGPUDevice>());
  const auto shader =
      handles_.add(Kind::ShaderModule, native<WGPUShaderModule>());
  constexpr uint32_t pipeline = 8;
  constexpr uint32_t vertex = pipeline + 16;
  constexpr uint32_t buffer = 128;
  constexpr uint32_t attribute = 160;
  constexpr uint32_t depth = 192;
  constexpr uint32_t fragment = 272;
  constexpr uint32_t target = 312;
  constexpr uint32_t entry = 384;
  put(pipeline + 72, depth);
  put(pipeline + 92, fragment);
  put(vertex + 4, shader);
  put(vertex + 8, entry);
  put(vertex + 12, UINT32_MAX);
  put(vertex + 24, 1);
  put(vertex + 28, buffer);
  putText(entry, "vs");
  put(buffer + 4, 2);  // Instance step mode.
  put<uint64_t>(buffer + 8, 16);
  put(buffer + 16, 1);
  put(buffer + 20, attribute);
  put(attribute + 4, 0x1e);  // Float32x3.
  put<uint64_t>(attribute + 8, 4);
  put(attribute + 16, 2);
  put(depth + 4, 0x2e);  // Depth24Plus.
  put(depth + 8, 1);
  put(depth + 12, 2);  // Less.
  put(fragment + 4, shader);
  put(fragment + 12, UINT32_MAX);
  put(fragment + 24, 1);
  put(fragment + 28, target);
  put(target + 4, 0x1b);  // BGRA8Unorm.
  put<uint64_t>(target + 16, 0xf);
  procs_.wgpuDeviceCreateRenderPipeline =
      [](WGPUDevice actual, const WGPURenderPipelineDescriptor* descriptor) {
        EXPECT_EQ(actual, native<WGPUDevice>());
        checkRenderPipeline(*descriptor);
        return native<WGPURenderPipeline>();
      };

  const auto result =
      invoke("wgpuDeviceCreateRenderPipeline", {word(device), word(pipeline)});
  EXPECT_EQ(handles_.get(Kind::RenderPipeline, result),
            native<WGPURenderPipeline>());
  put(attribute + 4, 0x7fffffffu);
  expectTrap("wgpuDeviceCreateRenderPipeline", {word(device), word(pipeline)},
             "WGPUVertexAttribute.format", "unknown");
}

auto checkBindGroup(const WGPUBindGroupDescriptor& descriptor) -> void {
  EXPECT_EQ(descriptor.layout, native<WGPUBindGroupLayout>());
  ASSERT_EQ(descriptor.entryCount, 2);
  ASSERT_NE(descriptor.entries, nullptr);
  EXPECT_EQ(descriptor.entries[0].binding, 2);
  EXPECT_EQ(descriptor.entries[0].buffer, native<WGPUBuffer>());
  EXPECT_EQ(descriptor.entries[0].offset, 256);
  EXPECT_EQ(descriptor.entries[0].size, WGPU_WHOLE_SIZE);
  EXPECT_EQ(descriptor.entries[0].sampler, nullptr);
  EXPECT_EQ(descriptor.entries[1].binding, 6);
  EXPECT_EQ(descriptor.entries[1].buffer, nullptr);
  EXPECT_EQ(descriptor.entries[1].sampler, native<WGPUSampler>());
  EXPECT_EQ(descriptor.entries[1].textureView, nullptr);
}

TEST_F(DecodeTest, BindGroupAndInvalidHandles) {
  const auto device = handles_.add(Kind::Device, native<WGPUDevice>());
  const auto layout =
      handles_.add(Kind::BindGroupLayout, native<WGPUBindGroupLayout>());
  const auto buffer = handles_.add(Kind::Buffer, native<WGPUBuffer>());
  const auto sampler = handles_.add(Kind::Sampler, native<WGPUSampler>());
  constexpr uint32_t descriptor = 8;
  constexpr uint32_t first = 64;
  constexpr uint32_t second = first + 40;
  put(descriptor + 12, layout);
  put(descriptor + 16, 2);
  put(descriptor + 20, first);
  put(first + 4, 2);
  put(first + 8, buffer);
  put<uint64_t>(first + 16, 256);
  put(first + 24, UINT64_MAX);
  put(second + 4, 6);
  put(second + 32, sampler);
  procs_.wgpuDeviceCreateBindGroup =
      [](WGPUDevice, const WGPUBindGroupDescriptor* descriptor) {
        checkBindGroup(*descriptor);
        return native<WGPUBindGroup>();
      };

  const auto group =
      invoke("wgpuDeviceCreateBindGroup", {word(device), word(descriptor)});
  EXPECT_EQ(handles_.get(Kind::BindGroup, group), native<WGPUBindGroup>());
  for (uint32_t invalid : {0u, 999u, buffer}) {
    put(descriptor + 12, invalid);
    expectTrap("wgpuDeviceCreateBindGroup", {word(device), word(descriptor)},
               "WGPUBindGroupDescriptor.layout", "handle id");
  }
  put(descriptor + 12, layout);
  put(descriptor + 16, UINT32_MAX);
  expectTrap("wgpuDeviceCreateBindGroup", {word(device), word(descriptor)},
             "WGPUBindGroupDescriptor.entries", "out of bounds");
}

auto checkWGSL(const WGPUShaderModuleDescriptor& descriptor) -> void {
  ASSERT_NE(descriptor.nextInChain, nullptr);
  EXPECT_EQ(descriptor.nextInChain->sType, WGPUSType_ShaderSourceWGSL);
  EXPECT_EQ(descriptor.nextInChain->next, nullptr);
  const auto* wgsl =
      reinterpret_cast<const WGPUShaderSourceWGSL*>(descriptor.nextInChain);
  EXPECT_EQ(std::string_view(wgsl->code.data, wgsl->code.length),
            "fn main() {}");
  EXPECT_EQ(std::string_view(descriptor.label.data, descriptor.label.length),
            "name");
}

TEST_F(DecodeTest, ShaderModuleAndForeignChains) {
  const auto device = handles_.add(Kind::Device, native<WGPUDevice>());
  constexpr uint32_t descriptor = 8;
  constexpr uint32_t chain = 32;
  put(descriptor, chain);
  put(descriptor + 4, 64);
  put(descriptor + 8, 4);
  putText(64, "name-extra");
  put(chain + 4, 2);  // WGPUSType_ShaderSourceWGSL.
  put(chain + 8, 80);
  put(chain + 12, UINT32_MAX);
  putText(80, "fn main() {}");
  procs_.wgpuDeviceCreateShaderModule =
      [](WGPUDevice, const WGPUShaderModuleDescriptor* descriptor) {
        checkWGSL(*descriptor);
        return static_cast<WGPUShaderModule>(nullptr);
      };

  EXPECT_EQ(
      invoke("wgpuDeviceCreateShaderModule", {word(device), word(descriptor)}),
      0);
  put(chain + 4, 1);  // WGPUSType_ShaderSourceSPIRV.
  expectTrap("wgpuDeviceCreateShaderModule", {word(device), word(descriptor)},
             "nextInChain", "expected WGPUShaderSourceWGSL");
  put(chain + 4, 2);
  put(chain, 0xfffffffcu);
  expectTrap("wgpuDeviceCreateShaderModule", {word(device), word(descriptor)},
             "nextInChain", "unexpected chained extension");
  put(chain, 0);
  put(chain + 8, 1023);
  putText(1023, "x");
  expectTrap("wgpuDeviceCreateShaderModule", {word(device), word(descriptor)},
             "WGPUShaderSourceWGSL.code", "unterminated string");

  procs_.wgpuDeviceCreateCommandEncoder =
      [](WGPUDevice, const WGPUCommandEncoderDescriptor*) {
        ADD_FAILURE() << "foreign chain should trap before the call";
        return static_cast<WGPUCommandEncoder>(nullptr);
      };
  expectTrap("wgpuDeviceCreateCommandEncoder", {word(device), word(descriptor)},
             "nextInChain", "unexpected chained extension");

  // The view ABI requires WGSL source for every shader module.
  put(descriptor, 0);
  expectTrap("wgpuDeviceCreateShaderModule", {word(device), word(descriptor)},
             "nextInChain", "missing WGPUShaderSourceWGSL");
}

TEST_F(DecodeTest, BindGroupLayoutRejectsBindingArrays) {
  const auto device = handles_.add(Kind::Device, native<WGPUDevice>());
  constexpr uint32_t descriptor = 8;
  constexpr uint32_t entry = 64;
  put(descriptor + 12, 1);  // entryCount
  put(descriptor + 16, entry);
  put(entry + 4, 3);  // binding
  put<uint64_t>(entry + 8, 2);  // visibility: fragment
  procs_.wgpuDeviceCreateBindGroupLayout =
      [](WGPUDevice, const WGPUBindGroupLayoutDescriptor* descriptor) {
        EXPECT_EQ(descriptor->entryCount, 1u);
        EXPECT_EQ(descriptor->entries[0].binding, 3u);
        EXPECT_EQ(descriptor->entries[0].bindingArraySize, 0u);
        return native<WGPUBindGroupLayout>();
      };
  const auto layout = invoke("wgpuDeviceCreateBindGroupLayout",
                             {word(device), word(descriptor)});
  EXPECT_EQ(handles_.get(Kind::BindGroupLayout, layout),
            native<WGPUBindGroupLayout>());
  put(entry + 16, 4);  // bindingArraySize
  expectTrap("wgpuDeviceCreateBindGroupLayout", {word(device), word(descriptor)},
             "WGPUBindGroupLayoutEntry.bindingArraySize", "binding arrays");
}

struct WriteRecord {
  const void* data = nullptr;
  size_t size = 0;
  uint64_t offset = 0;
};

TEST_F(DecodeTest, QueueWriteBufferBoundsAndBorrowedData) {
  WriteRecord recorded;
  const auto queue = handles_.add(Kind::Queue, &recorded);
  const auto buffer = handles_.add(Kind::Buffer, native<WGPUBuffer>());
  procs_.wgpuQueueWriteBuffer = [](WGPUQueue queue, WGPUBuffer buffer,
                                   uint64_t offset, const void* data,
                                   size_t size) {
    EXPECT_EQ(buffer, native<WGPUBuffer>());
    *reinterpret_cast<WriteRecord*>(queue) = {data, size, offset};
  };

  invoke("wgpuQueueWriteBuffer", {word(queue), word(buffer),
                                  Val{.i64 = 1LL << 34}, word(1020), word(4)});
  EXPECT_EQ(recorded.data, bytes_.data() + 1020);
  EXPECT_EQ(recorded.size, 4);
  EXPECT_EQ(recorded.offset, 1ULL << 34);
  expectTrap("wgpuQueueWriteBuffer",
             {word(queue), word(buffer), Val{.i64 = 0}, word(1020), word(5)},
             "data", "out of bounds");
  expectTrap(
      "wgpuQueueWriteBuffer",
      {word(queue), word(buffer), Val{.i64 = 0}, word(0xfffffffcu), word(8)},
      "data", "out of bounds");
  EXPECT_EQ(recorded.size, 4);
}

TEST_F(DecodeTest, ReleaseRemovesHandleButDestroyKeepsIt) {
  struct Calls {
    int destroys = 0;
    int releases = 0;
  } calls;
  const auto buffer = handles_.add(Kind::Buffer, &calls);
  procs_.wgpuBufferDestroy = [](WGPUBuffer buffer) {
    ++reinterpret_cast<Calls*>(buffer)->destroys;
  };
  procs_.wgpuBufferRelease = [](WGPUBuffer buffer) {
    ++reinterpret_cast<Calls*>(buffer)->releases;
  };

  invoke("wgpuBufferDestroy", {word(buffer)});
  EXPECT_EQ(calls.destroys, 1);
  EXPECT_EQ(handles_.get(Kind::Buffer, buffer), &calls);
  invoke("wgpuBufferRelease", {word(buffer)});
  EXPECT_EQ(calls.releases, 1);
  expectTrap("wgpuBufferRelease", {word(buffer)}, "buffer",
             "unknown WebGPU handle");
  EXPECT_EQ(calls.releases, 1);
  EXPECT_NE(handles_.add(Kind::Buffer, &calls), buffer);
}

}  // namespace
}  // namespace tsugumi::view
