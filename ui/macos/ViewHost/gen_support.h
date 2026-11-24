#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tsugumi::view {

struct Memory {
  uint8_t* base;
  uint64_t size;
};

struct TrapError : std::runtime_error {
  using runtime_error::runtime_error;
};

enum class Kind : uint8_t {
  BindGroup,
  BindGroupLayout,
  Buffer,
  CommandBuffer,
  CommandEncoder,
  ComputePassEncoder,
  ComputePipeline,
  Device,
  PipelineLayout,
  QuerySet,
  Queue,
  RenderPassEncoder,
  RenderPipeline,
  Sampler,
  ShaderModule,
  Texture,
  TextureView,
};

class Handles {
 public:
  struct Entry {
    Kind kind;
    void* native;
  };

  auto add(Kind kind, void* native) -> uint32_t {
    if (native == nullptr) return 0;
    if (next_ == 0) throw TrapError("WebGPU handle ids exhausted");
    const auto id = next_;
    entries_.emplace(id, Entry{kind, native});
    ++next_;
    return id;
  }

  auto get(Kind kind, uint32_t id) const -> void* {
    const auto it = entries_.find(id);
    if (it == entries_.end())
      throw TrapError("unknown WebGPU handle id " + std::to_string(id));
    if (it->second.kind != kind)
      throw TrapError("wrong kind for WebGPU handle id " + std::to_string(id));
    return it->second.native;
  }

  auto take(Kind kind, uint32_t id) -> void* {
    auto* native = get(kind, id);
    entries_.erase(id);
    return native;
  }

  auto takeIfPresent(Kind kind, uint32_t id) -> void* {
    return entries_.contains(id) ? take(kind, id) : nullptr;
  }

  // The host owns native release and can drain any handles left by the module
  // when dropping its instance.
  auto drain() -> std::unordered_map<uint32_t, Entry> {
    return std::exchange(entries_, {});
  }

 private:
  uint32_t next_ = 1;
  std::unordered_map<uint32_t, Entry> entries_;
};

class Arena {
 public:
  template <class T>
  auto allocate(size_t count = 1) -> T* {
    auto storage = std::make_shared_for_overwrite<T[]>(count);
    auto* data = storage.get();
    storage_.emplace_back(std::move(storage));
    return data;
  }

 private:
  std::vector<std::shared_ptr<void>> storage_;
};

enum class ValType : uint8_t { I32, I64, F32, F64 };

union Val {
  int32_t i32;
  int64_t i64;
  float f32;
  double f64;
};

}  // namespace tsugumi::view
