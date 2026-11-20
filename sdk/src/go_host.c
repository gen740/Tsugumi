/* Go uses the host call only while its guarded context is active. */
#include <string.h>

#include "host.h"

int tsugumi_go_check(const tsugumi_call* call) {
  return call && call->abi_version == TSUGUMI_ABI_VERSION;
}
const uint8_t* tsugumi_go_input(const tsugumi_call* call, const char* port,
                                size_t* size) {
  return call->effects->input(call->host, port, size);
}
int tsugumi_go_output(const tsugumi_call* call, const char* port,
                      const uint8_t* data, size_t size) {
  return call->effects->output(call->host, port, data, size);
}
const unsigned char* tsugumi_go_params(const tsugumi_call* call, size_t* size) {
  *size = call->params_size;
  return call->params;
}
const char* tsugumi_go_info(const tsugumi_call* call, unsigned field) {
  switch (field) {
    case 0:
      return call->run_id;
    case 1:
      return call->card;
    case 2:
      return call->working_directory;
    case 3:
      return call->temporary_directory;
    case 4:
      return call->output_directory;
    default:
      return NULL;
  }
}
uint64_t tsugumi_go_invocation(const tsugumi_call* call) {
  return call->invocation;
}
void tsugumi_go_log(const tsugumi_call* call, const char* text) {
  call->effects->log(call->host, text);
}
void tsugumi_go_fail(const tsugumi_call* call, const char* kind,
                     const char* text) {
  call->effects->fail(call->host, kind, text);
}
int tsugumi_go_cancelled(const tsugumi_call* call) {
  return call->effects->cancelled(call->host);
}
int64_t tsugumi_go_remaining(const tsugumi_call* call) {
  return call->effects->remaining_ms(call->host);
}
int tsugumi_go_text(const tsugumi_call* call, const char* text, int markdown) {
  return tsugumi_display_text(call, text, markdown);
}
int tsugumi_go_scalar(const tsugumi_call* call, double value,
                      const char* label) {
  return tsugumi_display_scalar(call, value, label);
}
int tsugumi_go_module(const tsugumi_call* call, const char* name,
                      const char* json) {
  return tsugumi_display_module(call, name, json, NULL, 0);
}
int tsugumi_go_image(const tsugumi_call* call, const char* media,
                     const unsigned char* bytes, size_t size, const char* alt) {
  return tsugumi_display_image(call, media, bytes, size, alt);
}
int tsugumi_go_table(const tsugumi_call* call, const char* const* columns,
                     size_t count, const char* const* cells, size_t rows) {
  return tsugumi_display_table(call, columns, count, cells, rows);
}
int tsugumi_go_line(const tsugumi_call* call, const char* id, const char* label,
                    const double* x, const double* y, size_t count) {
  tsugumi_series series = {id, label, x, y, count};
  return tsugumi_display_chart(call, &series, 1);
}
int tsugumi_go_points(const tsugumi_call* call, const float* x, const float* y,
                      const float* z, const uint32_t* colors, size_t count) {
  return tsugumi_display_points(call, x, y, z, colors, count);
}
