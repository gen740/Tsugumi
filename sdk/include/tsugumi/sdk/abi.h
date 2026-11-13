/* Tsugumi Card ABI. A Card package provides lib/main.so exporting
 * tsugumi_main. The worker owns every pointer passed in; they are valid only
 * during the call. Language runtimes stay with the resident worker.
 */
#ifndef TSUGUMI_SDK_ABI_H
#define TSUGUMI_SDK_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TSUGUMI_ABI_VERSION 5u

enum tsugumi_port_kind { TSUGUMI_FILE = 1, TSUGUMI_DIRECTORY = 2, TSUGUMI_FLATBUFFER = 3 };

typedef struct tsugumi_value {
  const char* name;      /* port name */
  uint32_t kind;         /* tsugumi_port_kind */
  const uint8_t* data;   /* FlatBuffer of the port message */
  size_t size;
} tsugumi_value;

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

typedef struct tsugumi_call {
  uint32_t abi_version;
  const char* run_id;
  const char* card;  /* full Card path joined with '/' */
  uint64_t invocation;
  const tsugumi_value* inputs;
  size_t input_count;
  const uint8_t* params; /* FlatBuffer of params.Params (_params.fbs) */
  size_t params_size;
  const char* working_directory;
  const char* temporary_directory;
  const char* output_directory;
  void* host;
  /* Emits one output port. Returns 0 on success. */
  int (*emit_output)(void* host, const char* port, const uint8_t* data, size_t size);
  int (*display_text)(void* host, const char* text);
  int (*display_scalar)(void* host, double value, const char* label);
  int (*display_line_chart)(void* host, const tsugumi_series* series, size_t count);
  /* Writes a line to the invocation's stderr log. */
  void (*log)(void* host, const char* message);
  /* Non-zero once the invocation was cancelled. */
  int (*cancelled)(void* host);
  /* Remaining milliseconds before the deadline, or -1 without deadline. */
  int64_t (*remaining_ms)(void* host);
  /* Records a structured failure; tsugumi_main should then return non-zero.
   * kind is a FailureKind name such as "worker_failed" or "output_invalid". */
  void (*fail)(void* host, const char* kind, const char* message);
  /* ABI 2. Table cells are row-major, column_count per row. */
  int (*display_table)(void* host, const char* const* columns, size_t column_count, const char* const* cells,
                       size_t row_count);
  /* PNG or JPEG bytes; media_type is "image/png" or "image/jpeg". */
  int (*display_image)(void* host, const char* media_type, const uint8_t* data, size_t size, const char* alt);
  /* colors (0xRRGGBB) may be NULL. */
  int (*display_points)(void* host, const float* x, const float* y, const float* z, const uint32_t* colors,
                        size_t count);
  /* Immutable view identity (or a name declared in views), config and named bytes. */
  int (*display_module)(void* host, const char* module, const char* config_json,
                        const tsugumi_blob* blobs, size_t blob_count);
  /* ABI 3. Markdown text (with TeX math) shown as formatted text. */
  int (*display_markdown)(void* host, const char* text);
  /* ABI 4. In-memory adapters for languages without a FlatBuffers generator.
   * Inputs/params are decoded using their actual schema; outputs are encoded
   * and validated as FlatBuffers. Returned strings last until the call ends.
   * NULL means an absent input (or a conversion failure, recorded by the host).
   * No JSON files or subprocess protocol are involved. */
  const char* (*input_json)(void* host, const char* port);
  const char* (*params_json)(void* host);
  int (*emit_json)(void* host, const char* port, const char* json);
  int (*display_json)(void* host, const char* json);

} tsugumi_call;

typedef int (*tsugumi_main_fn)(const tsugumi_call* call);

/* Implemented by the Card. Returns 0 on success. */
int tsugumi_main(const tsugumi_call* call);

#ifdef __cplusplus
}
#endif

#endif /* TSUGUMI_SDK_ABI_H */
