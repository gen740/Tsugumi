#pragma once
#include "abi.h"
#include <stdlib.h>

int tsugumi_go_check(const tsugumi_call *call);
uint64_t tsugumi_go_invocation(const tsugumi_call *call);
int tsugumi_go_emit(const tsugumi_call *call, const char *port,
                    const unsigned char *bytes, size_t size);
void tsugumi_go_log(const tsugumi_call *call, const char *text);
void tsugumi_go_fail(const tsugumi_call *call, const char *kind,
                     const char *text);
int tsugumi_go_cancelled(const tsugumi_call *call);
int64_t tsugumi_go_remaining(const tsugumi_call *call);
int tsugumi_go_text(const tsugumi_call *call, const char *text, int markdown);
int tsugumi_go_scalar(const tsugumi_call *call, double value,
                      const char *label);
int tsugumi_go_module(const tsugumi_call *call, const char *name,
                      const char *json);
int tsugumi_go_image(const tsugumi_call *call, const char *media,
                     const unsigned char *bytes, size_t size, const char *alt);
int tsugumi_go_table(const tsugumi_call *call, const char *const *columns,
                     size_t count, const char *const *cells, size_t rows);
int tsugumi_go_line(const tsugumi_call *call, const char *id, const char *label,
                    const double *x, const double *y, size_t count);
int tsugumi_go_points(const tsugumi_call *call, const float *x, const float *y,
                      const float *z, const uint32_t *colors, size_t count);
const unsigned char *tsugumi_go_input(const tsugumi_call *call,
                                      const char *name, size_t *size);
const unsigned char *tsugumi_go_params(const tsugumi_call *call, size_t *size);
const char *tsugumi_go_info(const tsugumi_call *call, unsigned field);
