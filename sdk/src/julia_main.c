/* Julia is initialized on, and always called from, the worker's main thread. */
#include "native_host.h"
#include <dlfcn.h>
#include <julia.h>
#include <stdlib.h>
#include <string.h>

static int check_exception(void) {
  if (!jl_exception_occurred()) return 0;
  fprintf(stderr, "Julia initialization failed: %s\n",
          jl_typeof_str(jl_exception_occurred()));
  return 1;
}
int tsugumi_load(void) {
  if (!dlopen(TSUGUMI_LIBJULIA, RTLD_NOW | RTLD_GLOBAL)) {
    fprintf(stderr, "%s\n", dlerror());
    return 1;
  }
  /* Capture only the pinned wrapper's runtime settings at build time. No
   * user environment or mutable package resolution is persisted here. */
  FILE* file = fopen(TSUGUMI_JULIA_ENV, "r");
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
  setenv("TSUGUMI_NATIVE_LIBRARY", TSUGUMI_SHIM, 1);
  jl_init();
  if (check_exception()) return 1;
  jl_eval_string("try; include(\"" TSUGUMI_JULIA_SDK
                 "\"); using .Tsugumi; include(\"" TSUGUMI_JULIA_SOURCE
                 "/main.jl\"); catch error; showerror(stderr, error, catch_backtrace()); println(stderr); rethrow(); end");
  return check_exception();
}
static int invoke(const tsugumi_call* call, int hook) {
  if (call->abi_version != TSUGUMI_ABI_VERSION) return 1;
  current_call = call;
  jl_value_t* result =
      jl_eval_string(hook ? "Tsugumi.invoke(true)" : "Tsugumi.invoke(false)");
  int ok = !check_exception() && result == jl_true;
  current_call = NULL;
  return ok ? 0 : 1;
}
int tsugumi_main(const tsugumi_call* call) { return invoke(call, 0); }
int tsugumi_run_start(const tsugumi_call* call) { return invoke(call, 1); }
