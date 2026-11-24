#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tsugumi_view_host tsugumi_view_host;

/* Called during a Wasm export on the calling thread. Return 0 after copying
 * the bytes, or a negative value if they could not be accepted. */
/* route points to the 16 network-order bytes of a route UUID. */
typedef int32_t (*tsugumi_view_host_send_fn)(void* context, const uint8_t* route,
                                            const uint8_t* bytes, uint32_t size);

/* Compile and validate the view, including abi(), without a GPU. */
__attribute__((visibility("default")))
int32_t tsugumi_view_host_check(const uint8_t* wasm, size_t size, char* error, size_t error_size);

/* Create a device and a surface on a CAMetalLayer*, configure the surface,
 * and call abi() and start(). The caller draws immediately after success. */
__attribute__((visibility("default")))
tsugumi_view_host* tsugumi_view_host_create(const uint8_t* wasm, size_t size, void* metal_layer,
    uint32_t width, uint32_t height, float scale, tsugumi_view_host_send_fn send,
    void* send_context, char* error, size_t error_size);

/* Draw and present. Negative means dropped. Bit 0 requests another draw. */
__attribute__((visibility("default")))
int32_t tsugumi_view_host_draw(tsugumi_view_host* host);

/* Reconfigure the surface before calling resize(). */
__attribute__((visibility("default")))
int32_t tsugumi_view_host_resize(tsugumi_view_host* host, uint32_t width, uint32_t height, float scale);

/* The message bytes are available to read() only during this call. */
__attribute__((visibility("default")))
int32_t tsugumi_view_host_message(tsugumi_view_host* host, const uint8_t* bytes, uint32_t size);

__attribute__((visibility("default")))
int32_t tsugumi_view_host_pointer(tsugumi_view_host* host, uint32_t kind, float x, float y,
    uint32_t buttons, float wheel_x, float wheel_y, uint32_t modifiers);
__attribute__((visibility("default")))
int32_t tsugumi_view_host_key(tsugumi_view_host* host, uint32_t kind, uint32_t code, uint32_t modifiers);

/* Bits 0, 1 and 2 indicate message, pointer and key respectively. */
__attribute__((visibility("default")))
uint32_t tsugumi_view_host_accepts(const tsugumi_view_host* host);

/* The error that dropped the view or its first WebGPU error, or NULL.
 * The pointer is valid until the next call. */
__attribute__((visibility("default")))
const char* tsugumi_view_host_error(const tsugumi_view_host* host);
__attribute__((visibility("default")))
void tsugumi_view_host_destroy(tsugumi_view_host* host);

#ifdef __cplusplus
}
#endif
