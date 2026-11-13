// Portable view boundary. GPU objects use the standard WebGPU C API.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TsugumiViewSize {
  float width, height, pixel_ratio;  // Logical pixels and device pixels per logical pixel.
} TsugumiViewSize;

typedef enum TsugumiPointerKind {
  TSUGUMI_POINTER_DOWN = 0,
  TSUGUMI_POINTER_MOVE = 1,
  TSUGUMI_POINTER_UP = 2,
  TSUGUMI_POINTER_CANCEL = 3
} TsugumiPointerKind;

// Borrowed, read-only bytes, valid until the next update or disposal. Config
// has a trailing NUL (excluded from size). A missing blob returns NULL/0.
const char* tsugumi_view_config(size_t* size);
const void* tsugumi_view_blob(const char* name, size_t* size);
void tsugumi_view_size(TsugumiViewSize* size);

// One callback per kind; NULL unregisters. Pointer coordinates and normalized
// wheel deltas are logical pixels. Pointer capture is handled by the host.
void tsugumi_view_on_pointer(void (*callback)(TsugumiPointerKind, float x, float y, uint32_t buttons));
void tsugumi_view_on_wheel(void (*callback)(float dx, float dy));
// Monotonic milliseconds. Frames pause while hidden; NULL stops animation.
void tsugumi_view_on_frame(void (*callback)(double milliseconds));
// Stops the view and reports a UTF-8 diagnostic to its host.
void tsugumi_view_error(const char* message);

// Implement these four entry points. Objects passed to initialise are borrowed
// until dispose returns; the host owns the device and surface. No main().
void tsugumi_view_initialise(WGPUDevice device, WGPUSurface surface, WGPUTextureFormat format);
void tsugumi_view_update(void);
void tsugumi_view_resize(void);
void tsugumi_view_dispose(void);

#ifdef __cplusplus
}
#endif
