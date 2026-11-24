#pragma once

// Temporary stand-in until the generator branch is integrated.
#include <cstdint>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace tsugumi::view {
struct Memory {
  uint8_t* base;
  uint64_t size;
};
struct TrapError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
enum class Kind {
  Device,
  Queue,
  TextureView,
  BindGroup,
  BindGroupLayout,
  Buffer,
  CommandBuffer,
  CommandEncoder,
  ComputePassEncoder,
  ComputePipeline,
  PipelineLayout,
  QuerySet,
  RenderPassEncoder,
  RenderPipeline,
  Sampler,
  ShaderModule,
  Texture
};
class Handles {
 public:
  auto add(Kind, void*) -> uint32_t { return 0; }
  auto takeIfPresent(Kind, uint32_t) noexcept -> void* { return nullptr; }
  struct Entry {
    Kind kind;
    void* native;
  };
  auto drain() -> std::unordered_map<uint32_t, Entry> { return {}; }
};
enum class ValType : uint8_t { I32, I64, F32, F64 };
union Val {
  int32_t i32;
  int64_t i64;
  float f32;
  double f64;
};
struct Procs {};
struct Context {
  Memory memory;
  Handles& handles;
  const Procs& procs;
};
struct Import {
  const char* name;
  std::span<const ValType> params;
  std::span<const ValType> results;
  void (*call)(Context&, const Val*, Val*);
};
inline auto dawnProcs() -> Procs { return {}; }
inline auto imports() -> std::span<const Import> { return {}; }
}  // namespace tsugumi::view
