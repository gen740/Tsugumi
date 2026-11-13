// The shared main module owns libc and Emdawnwebgpu; Cards are side modules.
#include <tsugumi/view.h>

extern "C" auto tsugumi_host_pointer(void (*callback)(TsugumiPointerKind, float, float, uint32_t),
                                   TsugumiPointerKind kind, float x, float y, uint32_t buttons) -> void {
  callback(kind, x, y, buttons);
}

extern "C" auto tsugumi_host_wheel(void (*callback)(float, float), float dx, float dy) -> void {
  callback(dx, dy);
}

extern "C" auto tsugumi_host_frame(void (*callback)(double), double time) -> void {
  callback(time);
}

extern "C" auto tsugumi_host_format(int bgra) -> WGPUTextureFormat {
  return bgra ? WGPUTextureFormat_BGRA8Unorm : WGPUTextureFormat_RGBA8Unorm;
}

extern "C" auto tsugumi_host_configure(WGPUDevice device, WGPUSurface surface,
                                     WGPUTextureFormat format, unsigned width, unsigned height) -> void {
  WGPUSurfaceConfiguration config{};
  config.device = device;
  config.format = format;
  config.usage = WGPUTextureUsage_RenderAttachment;
  config.width = width;
  config.height = height;
  config.presentMode = WGPUPresentMode_Fifo;
  config.alphaMode = WGPUCompositeAlphaMode_Opaque;
  wgpuSurfaceConfigure(surface, &config);
}
