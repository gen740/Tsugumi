/* Shared in-memory host surface for the R and Julia embedding shims. */
#pragma once
#include <stdio.h>
#include <string.h>

#include "tsugumi/sdk/abi.h"

static const tsugumi_call* current_call = NULL;

static const char* tsugumi_native_inputs(void) {
  return current_call ? current_call->effects->inputs_json(current_call->host) : NULL;
}
static const char* tsugumi_native_params(void) {
  return current_call ? current_call->effects->params_json(current_call->host) : NULL;
}
static const char* tsugumi_native_info(int field) {
  if (!current_call) return NULL;
  switch (field) {
    case 0:
      return current_call->run_id;
    case 1:
      return current_call->card;
    case 2:
      return current_call->working_directory;
    case 3:
      return current_call->temporary_directory;
    case 4:
      return current_call->output_directory;
    default:
      return NULL;
  }
}
static int tsugumi_native_input_kind(const char* port) {
  return current_call ? (int)current_call->effects->input_kind(current_call->host, port) : 0;
}
static uint64_t tsugumi_native_invocation(void) {
  return current_call ? current_call->invocation : 0;
}
static int tsugumi_native_display(const char* json) {
  return current_call ? current_call->effects->display_json(current_call->host, json)
                      : 1;
}
static int tsugumi_native_cancelled(void) {
  return !current_call || current_call->effects->cancelled(current_call->host);
}
static int64_t tsugumi_native_remaining_ms(void) {
  return current_call ? current_call->effects->remaining_ms(current_call->host) : 0;
}
static void tsugumi_native_log(const char* message) {
  if (current_call) current_call->effects->log(current_call->host, message);
}
static void tsugumi_native_fail(const char* message) {
  if (current_call) {
    current_call->effects->log(current_call->host, message);
    current_call->effects->fail(current_call->host, "worker_failed", message);
  } else {
    fprintf(stderr, "%s\n", message);
  }
}
