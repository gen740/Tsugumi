#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tsugumi_view_host tsugumi_view_host;
typedef struct tsugumi_view_stop tsugumi_view_stop;

typedef int32_t (*tsugumi_view_host_send_fn)(void* context, const char* to,
                                             const uint8_t* bytes,
                                             uint32_t size);
typedef void (*tsugumi_view_host_waiting_fn)(void* context, int32_t waiting);
typedef void (*tsugumi_view_host_pixels_fn)(void* context, const uint8_t* bgra,
                                            uint32_t width, uint32_t height,
                                            uint32_t stride);

enum {
  TSUGUMI_HOST_MESSAGE = 1u << 0,
  TSUGUMI_HOST_POINTER = 1u << 1,
  TSUGUMI_HOST_KEY = 1u << 2,
  TSUGUMI_HOST_AUDIO = 1u << 3,
  TSUGUMI_HOST_TEXT = 1u << 4,
  TSUGUMI_HOST_COMPOSITION = 1u << 5,
  TSUGUMI_HOST_GAMEPAD = 1u << 6,
};

typedef struct {
  float x, y, width, height;
} tsugumi_view_host_text_area;

/* The stop token outlives the host. Request may be called on any thread. */
__attribute__((visibility("default"))) tsugumi_view_stop*
tsugumi_view_stop_create(void);
__attribute__((visibility("default"))) void tsugumi_view_stop_request(
    tsugumi_view_stop* stop);
__attribute__((visibility("default"))) void tsugumi_view_stop_destroy(
    tsugumi_view_stop* stop);

/* Load and validate the view, including abi(), without a GPU. */
__attribute__((visibility("default"))) int32_t tsugumi_view_host_check(
    const uint8_t* wasm, size_t size, char* error, size_t error_size);

/* All host calls and callbacks run on the view thread. Copy callback data
 * before returning. Timestamps are seconds on the selected clock. A negative
 * time_origin selects the current AppKit monotonic uptime. */
__attribute__((visibility("default"))) tsugumi_view_host*
tsugumi_view_host_create(const uint8_t* wasm, size_t size, void* metal_layer,
                         uint32_t width, uint32_t height, float scale,
                         double time_origin,
                         tsugumi_view_stop* stop,
                         tsugumi_view_host_send_fn send,
                         tsugumi_view_host_waiting_fn waiting, void* context,
                         char* error, size_t error_size);

/* Negative means dropped. Bit 0 requests another draw. */
__attribute__((visibility("default"))) int32_t
tsugumi_view_host_draw(tsugumi_view_host* host, double present);
/* Snapshot returns -1 for a dropped instance and -2 for readback failure. */
__attribute__((visibility("default"))) int32_t
tsugumi_view_host_snapshot(tsugumi_view_host* host, double present,
                           tsugumi_view_host_pixels_fn pixels, void* context);
__attribute__((visibility("default"))) int32_t
tsugumi_view_host_resize(tsugumi_view_host* host, uint32_t width,
                         uint32_t height, float scale, double at);

__attribute__((visibility("default"))) int32_t tsugumi_view_host_message(
    tsugumi_view_host* host, const uint8_t* bytes, uint32_t size, double at);
__attribute__((visibility("default"))) int32_t tsugumi_view_host_pointer(
    tsugumi_view_host* host, uint32_t kind, float x, float y, uint32_t buttons,
    float wheel_x, float wheel_y, uint32_t modifiers, double at);
__attribute__((visibility("default"))) int32_t
tsugumi_view_host_key(tsugumi_view_host* host, uint32_t kind, uint32_t code,
                      uint32_t modifiers, double at);

__attribute__((visibility("default"))) int32_t tsugumi_view_host_text(
    tsugumi_view_host* host, const uint8_t* bytes, uint32_t size, double at);
__attribute__((visibility("default"))) int32_t
tsugumi_view_host_composition(tsugumi_view_host* host, const uint8_t* bytes,
                              uint32_t size, uint32_t cursor, double at);
__attribute__((visibility("default"))) int32_t tsugumi_view_host_text_input(
    const tsugumi_view_host* host, tsugumi_view_host_text_area* area);
__attribute__((visibility("default"))) int32_t tsugumi_view_host_gamepad(
    tsugumi_view_host* host, uint32_t pad, uint32_t kind, uint32_t buttons,
    float lx, float ly, float rx, float ry, float lt, float rt, double at);

/* Writes frames * 2 floats to the host's interleaved destination. */
__attribute__((visibility("default"))) int32_t tsugumi_view_host_audio(
    tsugumi_view_host* host, uint32_t frames, double heard, float* output);

/* Single producer (view thread) and single consumer (CoreAudio render thread).
 * The consumer combines the render timestamp with the output latency. */
typedef struct tsugumi_view_audio_ring tsugumi_view_audio_ring;
__attribute__((visibility("default"))) tsugumi_view_audio_ring*
tsugumi_view_audio_ring_create(void);
__attribute__((visibility("default"))) void tsugumi_view_audio_ring_destroy(
    tsugumi_view_audio_ring* ring);
__attribute__((visibility("default"))) int32_t tsugumi_view_audio_ring_fill(
    tsugumi_view_audio_ring* ring, tsugumi_view_host* host);
__attribute__((visibility("default"))) void tsugumi_view_audio_ring_set_latency(
    tsugumi_view_audio_ring* ring, double seconds);
__attribute__((visibility("default"))) void tsugumi_view_audio_ring_read(
    tsugumi_view_audio_ring* ring, float* left, float* right, uint32_t frames,
    double render_time);

__attribute__((visibility("default"))) uint32_t
tsugumi_view_host_accepts(const tsugumi_view_host* host);

/* The error that dropped the view or its first WebGPU error, or NULL.
 * The pointer is valid until the next call. */
__attribute__((visibility("default"))) const char* tsugumi_view_host_error(
    const tsugumi_view_host* host);
__attribute__((visibility("default"))) void tsugumi_view_host_destroy(
    tsugumi_view_host* host);

#ifdef __cplusplus
}
#endif
