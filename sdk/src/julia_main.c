/* Julia is initialized on, and always called from, the worker's main thread. */
#include <dlfcn.h>
#include <julia.h>
#include <stdlib.h>
#include <string.h>

#include "native_host.h"

extern const char tsugumi_julia_env[];
extern const char tsugumi_julia_source[];

static int check_exception(void) {
  if (!jl_exception_occurred()) return 0;
  fprintf(stderr, "Julia initialization failed: %s\n",
          jl_typeof_str(jl_exception_occurred()));
  return 1;
}
static int prepare(void) {
  if (!dlopen(TSUGUMI_LIBJULIA, RTLD_NOW | RTLD_GLOBAL)) {
    fprintf(stderr, "%s\n", dlerror());
    return 1;
  }
  /* Capture only the pinned wrapper's runtime settings at build time. No
   * user environment or mutable package resolution is persisted here. */
  FILE* file = fopen(tsugumi_julia_env, "r");
  if (!file) return 1;
  char* line = NULL;
  size_t capacity = 0;
  while (getline(&line, &capacity, file) >= 0) {
    line[strcspn(line, "\n")] = '\0';
    char* equal = strchr(line, '=');
    if (equal) {
      *equal = '\0';
      setenv(line, equal + 1, 1);
    }
  }
  free(line);
  fclose(file);
  /* Julia needs a writable first depot even for stdlib precompilation. HOME
   * is the worker's private, lifetime-scoped home, removed with the worker. */
  const char* home = getenv("HOME");
  const char* pinned = getenv("JULIA_DEPOT_PATH");
  if (!home) return 1;
  size_t depot_size = strlen(home) + (pinned ? strlen(pinned) : 0) + 10;
  char* depot = malloc(depot_size);
  if (!depot) return 1;
  snprintf(depot, depot_size, "%s/.julia:%s", home, pinned ? pinned : "");
  setenv("JULIA_DEPOT_PATH", depot, 1);
  free(depot);
  jl_init();
  if (check_exception()) return 1;
  // Julia calls private bridge functions by address so main.so exports
  // only the Card entry. The functions live for the interpreter's lifetime.
  char host[1024];
  snprintf(host, sizeof host,
           "const TSUGUMI_HOST = ("
           "inputs = Ptr{Cvoid}(%p), "
           "params = Ptr{Cvoid}(%p), "
           "info = Ptr{Cvoid}(%p), "
           "input_kind = Ptr{Cvoid}(%p), "
           "invocation = Ptr{Cvoid}(%p), "
           "display = Ptr{Cvoid}(%p), "
           "cancelled = Ptr{Cvoid}(%p), "
           "remaining_ms = Ptr{Cvoid}(%p), "
           "log = Ptr{Cvoid}(%p), "
           "fail = Ptr{Cvoid}(%p), "
           ")",
           (void*)tsugumi_native_inputs,
           (void*)tsugumi_native_params,
           (void*)tsugumi_native_info,
           (void*)tsugumi_native_input_kind,
           (void*)tsugumi_native_invocation,
           (void*)tsugumi_native_display,
           (void*)tsugumi_native_cancelled,
           (void*)tsugumi_native_remaining_ms,
           (void*)tsugumi_native_log,
           (void*)tsugumi_native_fail);
  jl_eval_string(host);
  if (check_exception()) return 1;
  jl_eval_string("try; include(\"" TSUGUMI_JULIA_SDK
                 "\"); using .Tsugumi; catch error; showerror(stderr, error, "
                 "catch_backtrace()); println(stderr); rethrow(); end");
  if (check_exception()) return 1;
  jl_eval_string("function _tsugumi_load_card(path); try; include(path); "
                 "catch error; showerror(stderr, error, catch_backtrace()); "
                 "println(stderr); rethrow(); end; end");
  return check_exception();
}
static int load(const tsugumi_call* call) {
  current_call = call;
  jl_value_t* load = jl_get_function(jl_main_module, "_tsugumi_load_card");
  jl_value_t* path = jl_cstr_to_string(tsugumi_julia_source);
  JL_GC_PUSH1(&path);
  jl_call1(load, path);
  JL_GC_POP();
  int error = check_exception();
  current_call = NULL;
  return error;
}
static int invoke(const tsugumi_call* call, tsugumi_buffer* outputs) {
  current_call = call;
  jl_value_t* result = jl_eval_string(
      outputs ? "Tsugumi.invoke(false)" : "Tsugumi.invoke(true)");
  int error = check_exception();
  if (!error) {
    if (outputs)
      error = !jl_is_string(result) ||
              call->effects->outputs_json(call->host, jl_string_ptr(result), outputs);
    else
      error = result != jl_true;
  }
  current_call = NULL;
  return error;
}
int tsugumi_card_invoke(const tsugumi_call* call, tsugumi_buffer* outputs) {
  switch (call->phase) {
    case TSUGUMI_PREPARE: return prepare();
    case TSUGUMI_LOAD: return load(call);
    case TSUGUMI_RUN_START: return invoke(call, NULL);
    case TSUGUMI_RUN: return invoke(call, outputs);
  }
  return 1;
}
