/* tsugumi_view.h — dawn view ABI, for wasm32 view modules */
#pragma once
#include <stdint.h>
#include <webgpu/webgpu.h>     /* pinned webgpu-headers revision (to be fixed with the host) */

#define TSUGUMI_VIEW_ABI 0      /* 0 until Tsugumi v1.0.0, like the Card ABI */
#define TSUGUMI_MESSAGE_MAX (1u << 24)  /* per-message limit, part of the Card ABI too */

/* ======================================================================
 * Imports: the ONLY functions a module may import. A module importing
 * anything else is rejected at load. No WASI, no files, no clock.
 * ==================================================================== */

/* module "webgpu": a subset of webgpu.h with the same signatures.
 * WGPU* handles are opaque i32 ids owned by the host.
 * TODO(open): exact webgpu imports fixed with the host: device/queue/buffer/
 * texture/sampler/shader module/bind group/pipeline/command encoder/render
 * pass, write, submit. */

/* module "tsugumi" */

/* Send bytes to the Card that owns this view. Never blocks.
 * The host copies the bytes before returning.
 * Returns 0 if queued, negative if the host cannot take it. */
int32_t tsugumi_view_send(const uint8_t* data, uint32_t size);

/* Copy the current message into dst. Valid only inside
 * tsugumi_view_message; size must equal the size it was given.
 * Returns 0 on success, negative otherwise. */
int32_t tsugumi_view_read(uint8_t* dst, uint32_t size);

/* ======================================================================
 * Exports: callbacks the host calls, found by name at load.
 * Return 0 on success; a negative value or a trap makes the host drop
 * this instance and show the error. One instance per view.
 * ==================================================================== */

/* --- required ---------------------------------------------------- */

/* (export "memory")  the module's linear memory, read by the host. */

/* Called first. Must return TSUGUMI_VIEW_ABI. */
int32_t tsugumi_view_abi(void);

/* Called once, for setup only: no target is given, so nothing is drawn here.
 * The host calls draw immediately after start returns.
 * format is the target texture format;
 * width/height in device pixels; scale = device pixels per logical pixel. */
int32_t tsugumi_view_start(WGPUDevice device, WGPUQueue queue,
                           WGPUTextureFormat format,
                           uint32_t width, uint32_t height, float scale);

/* Called when the drawing area changes size or scale. */
int32_t tsugumi_view_resize(uint32_t width, uint32_t height, float scale);

/* Draw into target. Return bit 0 set to ask to be drawn again;
 * otherwise the host calls draw again only after an event
 * (message, resize, pointer, key). Other bits are reserved (0). */
int32_t tsugumi_view_draw(WGPUTextureView target);

/* --- optional: data from the Card -------------------------------- */

/* One message the Card sent, of size bytes. Read it with
 * tsugumi_view_read into memory the view allocates itself.
 * Messages arrive in send order. */
int32_t tsugumi_view_message(uint32_t size);

/* --- optional: input --------------------------------------------- */

/* kind: 0 move, 1 down, 2 up, 3 wheel, 4 leave
 * x, y: logical pixels, origin at the top-left of the view
 * buttons: bit 0 primary, 1 secondary, 2 middle
 * modifiers: bit 0 shift, 1 control, 2 alt/option, 3 meta/command */
int32_t tsugumi_view_pointer(uint32_t kind, float x, float y,
                             uint32_t buttons,
                             float wheel_x, float wheel_y,
                             uint32_t modifiers);

/* kind: 0 down, 1 up; code: physical key, USB HID usage id */
int32_t tsugumi_view_key(uint32_t kind, uint32_t code, uint32_t modifiers);
