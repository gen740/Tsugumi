/* Tsugumi Card ABI. A Card package provides lib/main.so exporting
 * tsugumi_main. The worker owns the call and its data. They are valid only
 * during the call. Language runtimes stay with the resident worker.
 */
#ifndef TSUGUMI_SDK_ABI_H
#define TSUGUMI_SDK_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TSUGUMI_ABI_VERSION 7u

enum tsugumi_port_kind {
  TSUGUMI_FILE = 1,
  TSUGUMI_DIRECTORY = 2,
  TSUGUMI_FLATBUFFER = 3
};

typedef struct tsugumi_buffer {
  uint8_t* data;
  size_t size;
} tsugumi_buffer;

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

typedef struct tsugumi_host {
  int (*display_text)(void* host, const char* text);
  int (*display_scalar)(void* host, double value, const char* label);
  int (*display_line_chart)(void* host, const tsugumi_series* series,
                            size_t count);
  /* Writes a line to the invocation's stderr log. */
  void (*log)(void* host, const char* message);
  /* Non-zero once the invocation was cancelled. */
  int (*cancelled)(void* host);
  /* Remaining milliseconds before the deadline, or -1 without deadline. */
  int64_t (*remaining_ms)(void* host);
  /* Records a structured failure; tsugumi_main should then return non-zero.
   * kind is a FailureKind name such as "worker_failed" or "output_invalid". */
  void (*fail)(void* host, const char* kind, const char* message);
  /* Table cells are row-major, column_count per row. */
  int (*display_table)(void* host, const char* const* columns,
                       size_t column_count, const char* const* cells,
                       size_t row_count);
  /* PNG or JPEG bytes; media_type is "image/png" or "image/jpeg". */
  int (*display_image)(void* host, const char* media_type, const uint8_t* data,
                       size_t size, const char* alt);
  /* colors (0xRRGGBB) may be NULL. */
  int (*display_points)(void* host, const float* x, const float* y,
                        const float* z, const uint32_t* colors, size_t count);
  /* Immutable view identity (or a name declared in views), config and named
   * bytes. */
  int (*display_module)(void* host, const char* module, const char* config_json,
                        const tsugumi_blob* blobs, size_t blob_count);
  /* Markdown text (with TeX math) shown as formatted text. */
  int (*display_markdown)(void* host, const char* text);
  /* Typed JSON conversion in memory. Strings last through the call.
   * outputs_json fills an owned Outputs buffer
   * with the same allocation convention as tsugumi_main. */
  const char* (*inputs_json)(void* host);
  const char* (*params_json)(void* host);
  uint32_t (*input_kind)(void* host, const char* port);
  int (*outputs_json)(void* host, const char* json, tsugumi_buffer* outputs);
  int (*display_json)(void* host, const char* json);
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
  const uint8_t* inputs; /* FlatBuffer of inputs.Inputs (_inputs.fbs) */
  size_t inputs_size;
  const uint8_t* params; /* FlatBuffer of params.Params (_params.fbs) */
  size_t params_size;
  const char* working_directory;
  const char* temporary_directory;
  const char* output_directory;
  void* host;
  const tsugumi_host* effects;
} tsugumi_call;

/* Returns 0 on success. TSUGUMI_RUN produces one outputs.Outputs FlatBuffer
 * (_outputs.fbs) in outputs. Other phases leave outputs empty. The worker
 * frees outputs->data with free() after the call, even on failure. */
typedef int (*tsugumi_main_fn)(const tsugumi_call* call,
                               tsugumi_buffer* outputs);
int tsugumi_main(const tsugumi_call* call, tsugumi_buffer* outputs);

static inline int tsugumi_copy_outputs(tsugumi_buffer* outputs,
                                       const void* data, size_t size) {
  uint8_t* copy = (uint8_t*)malloc(size);
  if (!copy) return 1;
  memcpy(copy, data, size);
  outputs->data = copy;
  outputs->size = size;
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* TSUGUMI_SDK_ABI_H */
