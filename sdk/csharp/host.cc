// One CLR per worker. The managed assembly and host library live until exit.
#include <tsugumi/sdk/abi.h>

#include <coreclr_delegates.h>
#include <dlfcn.h>
#include <hostfxr.h>

#include <cstdio>
#include <cstdlib>

namespace {
using Entry = int (*)(const tsugumi_call *);
Entry g_main = nullptr;
Entry g_run_start = nullptr;
void *g_host = nullptr;

auto load_entry(load_assembly_and_get_function_pointer_fn load,
                const char *name, Entry *entry) -> bool {
  const int status = load(TSUGUMI_ASSEMBLY, "Tsugumi.Entry, Card", name,
                          UNMANAGEDCALLERSONLY_METHOD, nullptr,
                          reinterpret_cast<void **>(entry));
  if (status != 0)
    std::fprintf(stderr, "Cannot load C# entry %s (hostfxr status 0x%x)\n",
                 name, status);
  return status == 0;
}
} // namespace

extern "C" auto tsugumi_load() -> int {
  // A Card is not a debugging endpoint. No diagnostic socket is needed in its
  // filesystem/network sandbox; this does not change its application logging.
  ::setenv("DOTNET_EnableDiagnostics", "0", 1);
  g_host = ::dlopen(TSUGUMI_HOSTFXR, RTLD_NOW | RTLD_LOCAL);
  if (!g_host) {
    std::fprintf(stderr, "Cannot load .NET host: %s\n", ::dlerror());
    return 1;
  }
  const auto initialize =
      reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
          ::dlsym(g_host, "hostfxr_initialize_for_runtime_config"));
  const auto get_delegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
      ::dlsym(g_host, "hostfxr_get_runtime_delegate"));
  const auto close =
      reinterpret_cast<hostfxr_close_fn>(::dlsym(g_host, "hostfxr_close"));
  if (!initialize || !get_delegate || !close)
    return 1;
  hostfxr_initialize_parameters options{sizeof(hostfxr_initialize_parameters),
                                        nullptr, TSUGUMI_DOTNET_ROOT};
  hostfxr_handle handle = nullptr;
  const int status = initialize(TSUGUMI_RUNTIMECONFIG, &options, &handle);
  if (status < 0 || !handle) {
    std::fprintf(
        stderr,
        "Cannot initialize the pinned .NET runtime (hostfxr status 0x%x)\n",
        status);
    return 1;
  }
  void *pointer = nullptr;
  const int delegate_status = get_delegate(
      handle, hdt_load_assembly_and_get_function_pointer, &pointer);
  close(handle); // Closes configuration only, not the process-owned runtime.
  if (delegate_status != 0 || !pointer)
    return 1;
  const auto load =
      reinterpret_cast<load_assembly_and_get_function_pointer_fn>(pointer);
  return load_entry(load, "Execute", &g_main) &&
                 load_entry(load, "RunStart", &g_run_start)
             ? 0
             : 1;
}

extern "C" auto tsugumi_main(const tsugumi_call *call) -> int {
  return g_main ? g_main(call) : 1;
}
extern "C" auto tsugumi_run_start(const tsugumi_call *call) -> int {
  return g_run_start ? g_run_start(call) : 1;
}
