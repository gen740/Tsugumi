/* Shared in-memory host surface for the R and Julia embedding shims. */
#pragma once
#include "tsugumi/sdk/abi.h"
#include <stdio.h>
#include <string.h>

static const tsugumi_call* current_call = NULL;

const char* tsugumi_native_input(const char* port) {
  return current_call ? current_call->input_json(current_call->host, port)
                      : NULL;
}
const char* tsugumi_native_params(void) {
  return current_call ? current_call->params_json(current_call->host) : NULL;
}
const char* tsugumi_native_info(int field) {
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
int tsugumi_native_input_kind(const char* port) {
  if (current_call)
    for (size_t i = 0; i < current_call->input_count; ++i) {
      if (strcmp(current_call->inputs[i].name, port) == 0)
        return current_call->inputs[i].kind;
    }
  return 0;
}
uint64_t tsugumi_native_invocation(void) {
  return current_call ? current_call->invocation : 0;
}
int tsugumi_native_output(const char* port, const char* json) {
  return current_call ? current_call->emit_json(current_call->host, port, json)
                      : 1;
}
int tsugumi_native_display(const char* json) {
  return current_call ? current_call->display_json(current_call->host, json)
                      : 1;
}
int tsugumi_native_cancelled(void) {
  return !current_call || current_call->cancelled(current_call->host);
}
int64_t tsugumi_native_remaining_ms(void) {
  return current_call ? current_call->remaining_ms(current_call->host) : 0;
}
void tsugumi_native_fail(const char* message) {
  if (current_call) {
    current_call->log(current_call->host, message);
    current_call->fail(current_call->host, "worker_failed", message);
  } else {
    fprintf(stderr, "%s\n", message);
  }
}
