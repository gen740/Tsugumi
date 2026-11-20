/* Tsugumi Card ABI. A Card package provides lib/main.so exporting
 * tsugumi_main. The worker owns the call and its data. They are valid only
 * during the call. Language runtimes stay with the resident worker.
 */
#ifndef TSUGUMI_SDK_ABI_H
#define TSUGUMI_SDK_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TSUGUMI_ABI_VERSION 8u

enum tsugumi_port_kind {
  TSUGUMI_FILE = 1,
  TSUGUMI_DIRECTORY = 2,
  TSUGUMI_FLATBUFFER = 3
};

enum tsugumi_phase {
  TSUGUMI_PREPARE,
  TSUGUMI_LOAD,
  TSUGUMI_RUN_START,
  TSUGUMI_RUN
};

typedef struct tsugumi_series {
  const char* id;
  const char* label;
  const double* x;
  const double* y;
  size_t count;
} tsugumi_series;

typedef struct tsugumi_blob {
  const char* name;
  const uint8_t* data;
  size_t size;
} tsugumi_blob;

/* Text, Markdown and JSON carry a NUL-terminated string. The other kinds
 * carry pointers to their corresponding descriptor below. All data is
 * borrowed for the duration of the display callback. */
enum tsugumi_display_kind {
  TSUGUMI_DISPLAY_TEXT,
  TSUGUMI_DISPLAY_MARKDOWN,
  TSUGUMI_DISPLAY_SCALAR,
  TSUGUMI_DISPLAY_LINE_CHART,
  TSUGUMI_DISPLAY_TABLE,
  TSUGUMI_DISPLAY_IMAGE,
  TSUGUMI_DISPLAY_POINTS,
  TSUGUMI_DISPLAY_MODULE,
  TSUGUMI_DISPLAY_JSON
};

typedef struct tsugumi_scalar {
  double value;
  const char* label;
} tsugumi_scalar;

typedef struct tsugumi_chart {
  const tsugumi_series* series;
  size_t count;
} tsugumi_chart;

typedef struct tsugumi_table {
  const char* const* columns;
  size_t column_count;
  const char* const* cells; /* row-major */
  size_t row_count;
} tsugumi_table;

typedef struct tsugumi_image {
  const char* media_type;
  const uint8_t* data;
  size_t size;
  const char* alt;
} tsugumi_image;

typedef struct tsugumi_points {
  const float* x;
  const float* y;
  const float* z;
  const uint32_t* colors; /* optional (0xRRGGBB) */
  size_t count;
} tsugumi_points;

typedef struct tsugumi_module {
  const char* name; /* immutable identity, or a name declared in views */
  const char* config_json;
  const tsugumi_blob* blobs;
  size_t count;
} tsugumi_module;

typedef struct tsugumi_host {
  int (*display)(void* host, uint32_t kind, const void* data);
  /* Writes a line to the invocation's stderr log. */
  void (*log)(void* host, const char* message);
  /* Non-zero once the invocation was cancelled. */
  int (*cancelled)(void* host);
  /* Remaining milliseconds before the deadline, or -1 without deadline. */
  int64_t (*remaining_ms)(void* host);
  /* Records a structured failure; tsugumi_main should then return non-zero.
   * kind is a FailureKind name such as "worker_failed" or "output_invalid". */
  void (*fail)(void* host, const char* kind, const char* message);
  /* Returns a borrowed buffer, or NULL for an absent port. */
  const uint8_t* (*input)(void* host, const char* port, size_t* size);
  /* Send a port's buffer during TSUGUMI_RUN. The worker copies the bytes
   * before returning. Each port may be sent once. */
  int (*output)(void* host, const char* port, const uint8_t* data, size_t size);
  /* Typed JSON conversion for language adapters. Strings last through the call.
   */
  const char* (*inputs_json)(void* host);
  const char* (*params_json)(void* host);
  uint32_t (*input_kind)(void* host, const char* port);
  int (*output_json)(void* host, const char* port, const char* json);
  /* Failure at a source position. source is relative to the Card's
   * definition directory; line is one-based. cell may be NULL. */
  void (*fail_at)(void* host, const char* kind, const char* message,
                  const char* source, uint32_t line, const char* cell);

} tsugumi_host;

typedef struct tsugumi_call {
  uint32_t abi_version;
  uint32_t phase; /* tsugumi_phase */
  const char* run_id;
  const char* card; /* full Card path joined with '/' */
  uint64_t invocation;
  const uint8_t* params; /* FlatBuffer of parameters.Params (_params.fbs) */
  size_t params_size;
  const char* working_directory;
  const char* temporary_directory;
  const char* output_directory;
  void* host;
  const tsugumi_host* effects;
} tsugumi_call;

static inline int tsugumi_display_text(const tsugumi_call* call,
                                       const char* text, int markdown) {
  return call->effects->display(
      call->host, markdown ? TSUGUMI_DISPLAY_MARKDOWN : TSUGUMI_DISPLAY_TEXT,
      text);
}

static inline int tsugumi_display_scalar(const tsugumi_call* call, double value,
                                         const char* label) {
  const tsugumi_scalar data = {value, label};
  return call->effects->display(call->host, TSUGUMI_DISPLAY_SCALAR, &data);
}

static inline int tsugumi_display_chart(const tsugumi_call* call,
                                        const tsugumi_series* series,
                                        size_t count) {
  const tsugumi_chart data = {series, count};
  return call->effects->display(call->host, TSUGUMI_DISPLAY_LINE_CHART, &data);
}

static inline int tsugumi_display_table(const tsugumi_call* call,
                                        const char* const* columns,
                                        size_t column_count,
                                        const char* const* cells,
                                        size_t row_count) {
  const tsugumi_table data = {columns, column_count, cells, row_count};
  return call->effects->display(call->host, TSUGUMI_DISPLAY_TABLE, &data);
}

static inline int tsugumi_display_image(const tsugumi_call* call,
                                        const char* media_type,
                                        const uint8_t* bytes, size_t size,
                                        const char* alt) {
  const tsugumi_image data = {media_type, bytes, size, alt};
  return call->effects->display(call->host, TSUGUMI_DISPLAY_IMAGE, &data);
}

static inline int tsugumi_display_points(const tsugumi_call* call,
                                         const float* x, const float* y,
                                         const float* z, const uint32_t* colors,
                                         size_t count) {
  const tsugumi_points data = {x, y, z, colors, count};
  return call->effects->display(call->host, TSUGUMI_DISPLAY_POINTS, &data);
}

static inline int tsugumi_display_module(const tsugumi_call* call,
                                         const char* name, const char* json,
                                         const tsugumi_blob* blobs,
                                         size_t count) {
  const tsugumi_module data = {name, json, blobs, count};
  return call->effects->display(call->host, TSUGUMI_DISPLAY_MODULE, &data);
}

/* Returns 0 on success. Results are sent through the output callbacks. */
typedef int (*tsugumi_main_fn)(const tsugumi_call* call);
int tsugumi_main(const tsugumi_call* call);

#ifdef __cplusplus
}
#endif

#endif /* TSUGUMI_SDK_ABI_H */
