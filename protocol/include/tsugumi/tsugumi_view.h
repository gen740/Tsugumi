/* tsugumi_view.h — dawn view ABI, for wasm32 view modules */
#pragma once
#include <stdint.h>
#include <tsugumi/card_abi.h>  /* TSUGUMI_MESSAGE_MAX: the per-message limit */
/* webgpu.h of webgpu-headers, revision TSUGUMI_VIEW_WEBGPU_HEADERS_REV
 * (https://github.com/webgpu-native/webgpu-headers). The hosts translate
 * this header's wasm32 layout (pointers and size_t are 32-bit). */
#include <webgpu/webgpu.h>

#define TSUGUMI_VIEW_ABI 0      /* 0 until Tsugumi v1.0.0, like the Card ABI */
#define TSUGUMI_VIEW_WEBGPU_HEADERS_REV "a1dc77409bb2b8bd80540cbf4b8db219e123e909"

#if defined(__wasm__)
#define TSUGUMI_VIEW_IMPORT_(module, name) __attribute__((import_module(module), import_name(#name)))
#define TSUGUMI_VIEW_EXPORT_(name) __attribute__((export_name(#name)))
#else
#define TSUGUMI_VIEW_IMPORT_(module, name)
#define TSUGUMI_VIEW_EXPORT_(name)
#endif

/* ======================================================================
 * Imports: the ONLY functions a module may import. A module importing
 * anything else is rejected at load. No WASI, no files, no clock.
 * ==================================================================== */

/* module "webgpu": exactly these functions of webgpu.h, with its signatures.
 * WGPU* handles are opaque i32 ids owned by the host (0 is null); a view
 * receives the device and the queue in tsugumi_view_start and never
 * releases them. No function writes into the module's memory, and none takes
 * a callback. A WGPUShaderModuleDescriptor must chain exactly one
 * WGPUShaderSourceWGSL; every other nextInChain must be NULL. A
 * WGPUBindGroupLayoutEntry's bindingArraySize must be 0 (binding arrays are
 * not in this ABI). Breaking these rules is an error, as a trap is.
 * X(return type, name, (parameters)) */
#define TSUGUMI_VIEW_WEBGPU_IMPORTS(X) \
  X(WGPUBindGroup, wgpuDeviceCreateBindGroup, (WGPUDevice device, WGPUBindGroupDescriptor const * descriptor)) \
  X(WGPUBindGroupLayout, wgpuDeviceCreateBindGroupLayout, (WGPUDevice device, WGPUBindGroupLayoutDescriptor const * descriptor)) \
  X(WGPUBuffer, wgpuDeviceCreateBuffer, (WGPUDevice device, WGPUBufferDescriptor const * descriptor)) \
  X(WGPUCommandEncoder, wgpuDeviceCreateCommandEncoder, (WGPUDevice device, WGPUCommandEncoderDescriptor const * descriptor)) \
  X(WGPUComputePipeline, wgpuDeviceCreateComputePipeline, (WGPUDevice device, WGPUComputePipelineDescriptor const * descriptor)) \
  X(WGPUPipelineLayout, wgpuDeviceCreatePipelineLayout, (WGPUDevice device, WGPUPipelineLayoutDescriptor const * descriptor)) \
  X(WGPURenderPipeline, wgpuDeviceCreateRenderPipeline, (WGPUDevice device, WGPURenderPipelineDescriptor const * descriptor)) \
  X(WGPUSampler, wgpuDeviceCreateSampler, (WGPUDevice device, WGPUSamplerDescriptor const * descriptor)) \
  X(WGPUShaderModule, wgpuDeviceCreateShaderModule, (WGPUDevice device, WGPUShaderModuleDescriptor const * descriptor)) \
  X(WGPUTexture, wgpuDeviceCreateTexture, (WGPUDevice device, WGPUTextureDescriptor const * descriptor)) \
  X(void, wgpuQueueSubmit, (WGPUQueue queue, size_t commandCount, WGPUCommandBuffer const * commands)) \
  X(void, wgpuQueueWriteBuffer, (WGPUQueue queue, WGPUBuffer buffer, uint64_t bufferOffset, void const * data, size_t size)) \
  X(void, wgpuQueueWriteTexture, (WGPUQueue queue, WGPUTexelCopyTextureInfo const * destination, void const * data, size_t dataSize, WGPUTexelCopyBufferLayout const * dataLayout, WGPUExtent3D const * writeSize)) \
  X(void, wgpuBufferDestroy, (WGPUBuffer buffer)) \
  X(void, wgpuBufferRelease, (WGPUBuffer buffer)) \
  X(WGPUTextureView, wgpuTextureCreateView, (WGPUTexture texture, WGPUTextureViewDescriptor const * descriptor)) \
  X(void, wgpuTextureDestroy, (WGPUTexture texture)) \
  X(void, wgpuTextureRelease, (WGPUTexture texture)) \
  X(void, wgpuTextureViewRelease, (WGPUTextureView textureView)) \
  X(void, wgpuSamplerRelease, (WGPUSampler sampler)) \
  X(void, wgpuShaderModuleRelease, (WGPUShaderModule shaderModule)) \
  X(void, wgpuBindGroupRelease, (WGPUBindGroup bindGroup)) \
  X(void, wgpuBindGroupLayoutRelease, (WGPUBindGroupLayout bindGroupLayout)) \
  X(void, wgpuPipelineLayoutRelease, (WGPUPipelineLayout pipelineLayout)) \
  X(WGPUBindGroupLayout, wgpuRenderPipelineGetBindGroupLayout, (WGPURenderPipeline renderPipeline, uint32_t groupIndex)) \
  X(void, wgpuRenderPipelineRelease, (WGPURenderPipeline renderPipeline)) \
  X(WGPUBindGroupLayout, wgpuComputePipelineGetBindGroupLayout, (WGPUComputePipeline computePipeline, uint32_t groupIndex)) \
  X(void, wgpuComputePipelineRelease, (WGPUComputePipeline computePipeline)) \
  X(WGPUComputePassEncoder, wgpuCommandEncoderBeginComputePass, (WGPUCommandEncoder commandEncoder, WGPUComputePassDescriptor const * descriptor)) \
  X(WGPURenderPassEncoder, wgpuCommandEncoderBeginRenderPass, (WGPUCommandEncoder commandEncoder, WGPURenderPassDescriptor const * descriptor)) \
  X(void, wgpuCommandEncoderCopyBufferToBuffer, (WGPUCommandEncoder commandEncoder, WGPUBuffer source, uint64_t sourceOffset, WGPUBuffer destination, uint64_t destinationOffset, uint64_t size)) \
  X(WGPUCommandBuffer, wgpuCommandEncoderFinish, (WGPUCommandEncoder commandEncoder, WGPUCommandBufferDescriptor const * descriptor)) \
  X(void, wgpuCommandEncoderRelease, (WGPUCommandEncoder commandEncoder)) \
  X(void, wgpuCommandBufferRelease, (WGPUCommandBuffer commandBuffer)) \
  X(void, wgpuComputePassEncoderDispatchWorkgroups, (WGPUComputePassEncoder computePassEncoder, uint32_t workgroupCountX, uint32_t workgroupCountY, uint32_t workgroupCountZ)) \
  X(void, wgpuComputePassEncoderEnd, (WGPUComputePassEncoder computePassEncoder)) \
  X(void, wgpuComputePassEncoderRelease, (WGPUComputePassEncoder computePassEncoder)) \
  X(void, wgpuComputePassEncoderSetBindGroup, (WGPUComputePassEncoder computePassEncoder, uint32_t groupIndex, WGPUBindGroup group, size_t dynamicOffsetCount, uint32_t const * dynamicOffsets)) \
  X(void, wgpuComputePassEncoderSetPipeline, (WGPUComputePassEncoder computePassEncoder, WGPUComputePipeline pipeline)) \
  X(void, wgpuRenderPassEncoderDraw, (WGPURenderPassEncoder renderPassEncoder, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)) \
  X(void, wgpuRenderPassEncoderDrawIndexed, (WGPURenderPassEncoder renderPassEncoder, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance)) \
  X(void, wgpuRenderPassEncoderEnd, (WGPURenderPassEncoder renderPassEncoder)) \
  X(void, wgpuRenderPassEncoderRelease, (WGPURenderPassEncoder renderPassEncoder)) \
  X(void, wgpuRenderPassEncoderSetBindGroup, (WGPURenderPassEncoder renderPassEncoder, uint32_t groupIndex, WGPUBindGroup group, size_t dynamicOffsetCount, uint32_t const * dynamicOffsets)) \
  X(void, wgpuRenderPassEncoderSetIndexBuffer, (WGPURenderPassEncoder renderPassEncoder, WGPUBuffer buffer, WGPUIndexFormat format, uint64_t offset, uint64_t size)) \
  X(void, wgpuRenderPassEncoderSetPipeline, (WGPURenderPassEncoder renderPassEncoder, WGPURenderPipeline pipeline)) \
  X(void, wgpuRenderPassEncoderSetScissorRect, (WGPURenderPassEncoder renderPassEncoder, uint32_t x, uint32_t y, uint32_t width, uint32_t height)) \
  X(void, wgpuRenderPassEncoderSetVertexBuffer, (WGPURenderPassEncoder renderPassEncoder, uint32_t slot, WGPUBuffer buffer, uint64_t offset, uint64_t size)) \
  X(void, wgpuRenderPassEncoderSetViewport, (WGPURenderPassEncoder renderPassEncoder, float x, float y, float width, float height, float minDepth, float maxDepth))

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__wasm__)
/* Using a webgpu.h function outside this list leaves an undefined symbol. */
#define TSUGUMI_VIEW_DECLARE_IMPORT_(ret, name, params) TSUGUMI_VIEW_IMPORT_("webgpu", name) ret name params;
TSUGUMI_VIEW_WEBGPU_IMPORTS(TSUGUMI_VIEW_DECLARE_IMPORT_)
#undef TSUGUMI_VIEW_DECLARE_IMPORT_
#endif

/* module "tsugumi" */

/* Send bytes to a route of this window. A route of all zero bytes means
 * the route returned by view_open. The host rejects a route from another
 * window and drops messages for a dead route. Never blocks. The host
 * copies the route and data before returning. Returns 0 when accepted
 * and a negative value when the host cannot accept the message. */
TSUGUMI_VIEW_IMPORT_("tsugumi", tsugumi_view_send)
int32_t tsugumi_view_send(const uint8_t* route /* 16 bytes */,
                          const uint8_t* data, uint32_t size);

/* Copy the current message into dst. Valid only inside
 * tsugumi_view_message; size must equal the size it was given.
 * Returns 0 on success, negative otherwise. */
TSUGUMI_VIEW_IMPORT_("tsugumi", tsugumi_view_read)
int32_t tsugumi_view_read(uint8_t* dst, uint32_t size);

/* ======================================================================
 * Exports: callbacks the host calls, found by name at load.
 * Return 0 on success; a negative value or a trap makes the host drop
 * this instance and show the error. One instance per view.
 * A WebGPU validation error or device loss does not drop the instance:
 * the host shows the first one under the view and keeps calling it.
 * ==================================================================== */

/* --- required ---------------------------------------------------- */

/* (export "memory")  the module's linear memory, read by the host. */

/* Called first. Must return TSUGUMI_VIEW_ABI. */
TSUGUMI_VIEW_EXPORT_(tsugumi_view_abi)
int32_t tsugumi_view_abi(void);

/* Called once, for setup only: no target is given, so nothing is drawn here.
 * The host calls draw immediately after start returns.
 * format is the target texture format;
 * width/height in device pixels; scale = device pixels per logical pixel. */
TSUGUMI_VIEW_EXPORT_(tsugumi_view_start)
int32_t tsugumi_view_start(WGPUDevice device, WGPUQueue queue,
                           WGPUTextureFormat format,
                           uint32_t width, uint32_t height, float scale);

/* Called when the drawing area changes size or scale. */
TSUGUMI_VIEW_EXPORT_(tsugumi_view_resize)
int32_t tsugumi_view_resize(uint32_t width, uint32_t height, float scale);

/* Draw into target. Return bit 0 set to ask to be drawn again;
 * otherwise the host calls draw again only after an event
 * (message, resize, pointer, key). Other bits are reserved (0). */
TSUGUMI_VIEW_EXPORT_(tsugumi_view_draw)
int32_t tsugumi_view_draw(WGPUTextureView target);

/* --- optional: data from the Card -------------------------------- */

/* One message the Card sent, of size bytes. Read it with
 * tsugumi_view_read into memory the view allocates itself.
 * Messages arrive in send order. */
TSUGUMI_VIEW_EXPORT_(tsugumi_view_message)
int32_t tsugumi_view_message(uint32_t size);

/* --- optional: input --------------------------------------------- */

/* kind: 0 move, 1 down, 2 up, 3 wheel, 4 leave
 * x, y: logical pixels, origin at the top-left of the view
 * buttons: bit 0 primary, 1 secondary, 2 middle
 * modifiers: bit 0 shift, 1 control, 2 alt/option, 3 meta/command */
TSUGUMI_VIEW_EXPORT_(tsugumi_view_pointer)
int32_t tsugumi_view_pointer(uint32_t kind, float x, float y,
                             uint32_t buttons,
                             float wheel_x, float wheel_y,
                             uint32_t modifiers);

/* kind: 0 down, 1 up; code: physical key, USB HID usage id */
TSUGUMI_VIEW_EXPORT_(tsugumi_view_key)
int32_t tsugumi_view_key(uint32_t kind, uint32_t code, uint32_t modifiers);

#ifdef __cplusplus
}
#endif
