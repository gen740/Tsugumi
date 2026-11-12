// One CLR per worker. The managed assembly and host library live until exit.
#include <tsugumi/sdk/abi.h>

#include <coreclr_delegates.h>
#include <dlfcn.h>
#include <hostfxr.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
using Entry = int (*)(const tsugumi_call *);
Entry g_main = nullptr;
Entry g_run_start = nullptr;
void *g_host = nullptr;

// The ABI structs as this compiler lays them out from abi.h. The managed
// mirror (sdk/csharp/Context.cs) is checked against it before the first call
// (Layout.cs), so a field added to abi.h without its C# counterpart
// stops the Card at load with a layout error.
// Every field of every struct the worker passes to the Card is listed.
struct Field {
  const char *name;
  uint64_t offset;
  uint64_t size;
};
#define TSUGUMI_STRUCT(type) Field{#type, 0, sizeof(type)}
#define TSUGUMI_FIELD(type, member)                                            \
  Field { #type "." #member, offsetof(type, member), sizeof(type::member) }
constexpr Field k_layout[] = {
    TSUGUMI_STRUCT(tsugumi_value),
    TSUGUMI_FIELD(tsugumi_value, name),
    TSUGUMI_FIELD(tsugumi_value, kind),
    TSUGUMI_FIELD(tsugumi_value, data),
    TSUGUMI_FIELD(tsugumi_value, size),
    TSUGUMI_STRUCT(tsugumi_series),
    TSUGUMI_FIELD(tsugumi_series, id),
    TSUGUMI_FIELD(tsugumi_series, label),
    TSUGUMI_FIELD(tsugumi_series, x),
    TSUGUMI_FIELD(tsugumi_series, y),
    TSUGUMI_FIELD(tsugumi_series, count),
    TSUGUMI_STRUCT(tsugumi_call),
    TSUGUMI_FIELD(tsugumi_call, abi_version),
    TSUGUMI_FIELD(tsugumi_call, run_id),
    TSUGUMI_FIELD(tsugumi_call, card),
    TSUGUMI_FIELD(tsugumi_call, invocation),
    TSUGUMI_FIELD(tsugumi_call, inputs),
    TSUGUMI_FIELD(tsugumi_call, input_count),
    TSUGUMI_FIELD(tsugumi_call, params),
    TSUGUMI_FIELD(tsugumi_call, params_size),
    TSUGUMI_FIELD(tsugumi_call, working_directory),
    TSUGUMI_FIELD(tsugumi_call, temporary_directory),
    TSUGUMI_FIELD(tsugumi_call, output_directory),
    TSUGUMI_FIELD(tsugumi_call, host),
    TSUGUMI_FIELD(tsugumi_call, emit_output),
    TSUGUMI_FIELD(tsugumi_call, display_text),
    TSUGUMI_FIELD(tsugumi_call, display_scalar),
    TSUGUMI_FIELD(tsugumi_call, display_line_chart),
    TSUGUMI_FIELD(tsugumi_call, log),
    TSUGUMI_FIELD(tsugumi_call, cancelled),
    TSUGUMI_FIELD(tsugumi_call, remaining_ms),
    TSUGUMI_FIELD(tsugumi_call, fail),
    TSUGUMI_FIELD(tsugumi_call, display_table),
    TSUGUMI_FIELD(tsugumi_call, display_image),
    TSUGUMI_FIELD(tsugumi_call, display_points),
    TSUGUMI_FIELD(tsugumi_call, display_module),
    TSUGUMI_FIELD(tsugumi_call, display_markdown),
    TSUGUMI_FIELD(tsugumi_call, input_json),
    TSUGUMI_FIELD(tsugumi_call, params_json),
    TSUGUMI_FIELD(tsugumi_call, emit_json),
    TSUGUMI_FIELD(tsugumi_call, display_json),
};
#undef TSUGUMI_FIELD
#undef TSUGUMI_STRUCT
using CheckLayout = int (*)(const Field *, size_t);

auto load_entry(load_assembly_and_get_function_pointer_fn load,
                const char *name, void **entry) -> bool {
  const int status = load(TSUGUMI_ASSEMBLY, "Tsugumi.Entry, Card", name,
                          UNMANAGEDCALLERSONLY_METHOD, nullptr, entry);
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
  void *check = nullptr;
  if (!load_entry(load, "CheckLayout", &check) ||
      reinterpret_cast<CheckLayout>(check)(
          k_layout, sizeof k_layout / sizeof k_layout[0]) != 0) {
    std::fprintf(stderr, "The C# SDK does not match this worker's Card ABI "
                         "(tsugumi/sdk/abi.h); see above\n");
    return 1;
  }
  return load_entry(load, "Execute", reinterpret_cast<void **>(&g_main)) &&
                 load_entry(load, "RunStart",
                            reinterpret_cast<void **>(&g_run_start))
             ? 0
             : 1;
}

extern "C" auto tsugumi_main(const tsugumi_call *call) -> int {
  return g_main ? g_main(call) : 1;
}
extern "C" auto tsugumi_run_start(const tsugumi_call *call) -> int {
  return g_run_start ? g_run_start(call) : 1;
}
