/* Tsugumi Display renderer (Dawn/WebGPU on Metal). C API for the SwiftUI bridge.
 *
 * Threading: create, resize, set_* and render must be called from one thread
 * (the main thread). set_* only records data; render draws at most one frame
 * when something changed, so bursts of updates are aggregated per frame. */
#ifndef TSUGUMI_RENDERER_H
#define TSUGUMI_RENDERER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TSG_RENDERER_EXPORT __attribute__((visibility("default")))

typedef struct tsg_renderer tsg_renderer;

typedef struct tsg_series {
  const double* x;
  const double* y;
  size_t count;
  /* line color, linear sRGB */
  float r;
  float g;
  float b;
  float a;
} tsg_series;

typedef struct tsg_renderer_stats {
  uint64_t frames;          /* frames presented */
  uint64_t updates;         /* set_* calls */
  uint64_t vertex_bytes;    /* current GPU vertex buffer size */
  uint32_t width;           /* surface width in pixels */
  uint32_t height;          /* surface height in pixels */
} tsg_renderer_stats;

/* `metal_layer` is a CAMetalLayer*. Returns NULL and fills `error` on failure. */
TSG_RENDERER_EXPORT tsg_renderer* tsg_renderer_create(void* metal_layer, char* error, size_t error_len);
TSG_RENDERER_EXPORT void tsg_renderer_destroy(tsg_renderer* r);
/* Size in pixels (points * scale). */
TSG_RENDERER_EXPORT void tsg_renderer_resize(tsg_renderer* r, uint32_t width, uint32_t height, float scale);
/* Replaces the whole chart (full snapshot). Data is copied. */
TSG_RENDERER_EXPORT void tsg_renderer_set_line_chart(tsg_renderer* r, const tsg_series* series, size_t count);
TSG_RENDERER_EXPORT void tsg_renderer_set_background(tsg_renderer* r, float red, float green, float blue);
/* Draws one frame when dirty. Returns 1 when a frame was presented. */
TSG_RENDERER_EXPORT int tsg_renderer_render(tsg_renderer* r);
TSG_RENDERER_EXPORT void tsg_renderer_stats_get(const tsg_renderer* r, tsg_renderer_stats* out);

#ifdef __cplusplus
}
#endif

#endif /* TSUGUMI_RENDERER_H */
