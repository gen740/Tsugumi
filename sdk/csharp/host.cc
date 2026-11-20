// One CLR per worker. The managed assembly and host library live until exit.
#include <coreclr_delegates.h>
#include <dlfcn.h>
#include <hostfxr.h>
#include <tsugumi/sdk/abi.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {
extern const char tsugumi_csharp_assembly[];
extern const char tsugumi_csharp_runtimeconfig[];
}

namespace {
using Entry = int (*)(const tsugumi_call*);
load_assembly_and_get_function_pointer_fn g_load = nullptr;
Entry g_main = nullptr;
void* g_host = nullptr;

// The ABI structs as this compiler lays them out from abi.h. The managed
// mirror (sdk/csharp/InvocationScope.cs) is checked against it before the first
// call (Layout.cs), so a field added to abi.h without its C# counterpart stops
// the Card at load with a layout error. Every field of every struct the worker
// passes to the Card is listed.
struct Field {
  const char* name;
  uint64_t offset;
  uint64_t size;
};
#define TSUGUMI_STRUCT(type) \
  Field { #type, 0, sizeof(type) }
#define TSUGUMI_FIELD(type, member) \
  Field { #type "." #member, offsetof(type, member), sizeof(type::member) }
constexpr Field k_layout[] = {
    TSUGUMI_STRUCT(tsugumi_blob),
    TSUGUMI_FIELD(tsugumi_blob, name),
    TSUGUMI_FIELD(tsugumi_blob, data),
    TSUGUMI_FIELD(tsugumi_blob, size),
    TSUGUMI_STRUCT(tsugumi_series),
    TSUGUMI_FIELD(tsugumi_series, id),
    TSUGUMI_FIELD(tsugumi_series, label),
    TSUGUMI_FIELD(tsugumi_series, x),
    TSUGUMI_FIELD(tsugumi_series, y),
    TSUGUMI_FIELD(tsugumi_series, count),
    TSUGUMI_STRUCT(tsugumi_scalar),
    TSUGUMI_FIELD(tsugumi_scalar, value),
    TSUGUMI_FIELD(tsugumi_scalar, label),
    TSUGUMI_STRUCT(tsugumi_chart),
    TSUGUMI_FIELD(tsugumi_chart, series),
    TSUGUMI_FIELD(tsugumi_chart, count),
    TSUGUMI_STRUCT(tsugumi_table),
    TSUGUMI_FIELD(tsugumi_table, columns),
    TSUGUMI_FIELD(tsugumi_table, column_count),
    TSUGUMI_FIELD(tsugumi_table, cells),
    TSUGUMI_FIELD(tsugumi_table, row_count),
    TSUGUMI_STRUCT(tsugumi_image),
    TSUGUMI_FIELD(tsugumi_image, media_type),
    TSUGUMI_FIELD(tsugumi_image, data),
    TSUGUMI_FIELD(tsugumi_image, size),
    TSUGUMI_FIELD(tsugumi_image, alt),
    TSUGUMI_STRUCT(tsugumi_points),
    TSUGUMI_FIELD(tsugumi_points, x),
    TSUGUMI_FIELD(tsugumi_points, y),
    TSUGUMI_FIELD(tsugumi_points, z),
    TSUGUMI_FIELD(tsugumi_points, colors),
    TSUGUMI_FIELD(tsugumi_points, count),
    TSUGUMI_STRUCT(tsugumi_module),
    TSUGUMI_FIELD(tsugumi_module, name),
    TSUGUMI_FIELD(tsugumi_module, config_json),
    TSUGUMI_FIELD(tsugumi_module, blobs),
    TSUGUMI_FIELD(tsugumi_module, count),
    TSUGUMI_STRUCT(tsugumi_call),
    TSUGUMI_FIELD(tsugumi_call, abi_version),
    TSUGUMI_FIELD(tsugumi_call, phase),
    TSUGUMI_FIELD(tsugumi_call, run_id),
    TSUGUMI_FIELD(tsugumi_call, card),
    TSUGUMI_FIELD(tsugumi_call, invocation),
    TSUGUMI_FIELD(tsugumi_call, params),
    TSUGUMI_FIELD(tsugumi_call, params_size),
    TSUGUMI_FIELD(tsugumi_call, working_directory),
    TSUGUMI_FIELD(tsugumi_call, temporary_directory),
    TSUGUMI_FIELD(tsugumi_call, output_directory),
    TSUGUMI_FIELD(tsugumi_call, host),
    TSUGUMI_FIELD(tsugumi_call, effects),
    TSUGUMI_STRUCT(tsugumi_host),
    TSUGUMI_FIELD(tsugumi_host, display),
    TSUGUMI_FIELD(tsugumi_host, log),
    TSUGUMI_FIELD(tsugumi_host, cancelled),
    TSUGUMI_FIELD(tsugumi_host, remaining_ms),
    TSUGUMI_FIELD(tsugumi_host, fail),
    TSUGUMI_FIELD(tsugumi_host, input),
    TSUGUMI_FIELD(tsugumi_host, output),
    TSUGUMI_FIELD(tsugumi_host, inputs_json),
    TSUGUMI_FIELD(tsugumi_host, params_json),
    TSUGUMI_FIELD(tsugumi_host, input_kind),
    TSUGUMI_FIELD(tsugumi_host, output_json),
    TSUGUMI_FIELD(tsugumi_host, fail_at),
};
#undef TSUGUMI_FIELD
#undef TSUGUMI_STRUCT
using CheckLayout = int (*)(const Field*, size_t);

auto load_entry(load_assembly_and_get_function_pointer_fn load,
                const char* name, void** entry) -> bool {
  const int status = load(tsugumi_csharp_assembly, "Tsugumi.Entry, Card", name,
                          UNMANAGEDCALLERSONLY_METHOD, nullptr, entry);
  if (status != 0)
    std::fprintf(stderr, "Cannot load C# entry %s (hostfxr status 0x%x)\n",
                 name, status);
  return status == 0;
}
}  // namespace

static auto prepare() -> int {
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
  if (!initialize || !get_delegate || !close) return 1;
  hostfxr_initialize_parameters options{sizeof(hostfxr_initialize_parameters),
                                        nullptr, TSUGUMI_DOTNET_ROOT};
  hostfxr_handle handle = nullptr;
  const int status =
      initialize(tsugumi_csharp_runtimeconfig, &options, &handle);
  if (status < 0 || !handle) {
    std::fprintf(
        stderr,
        "Cannot initialize the pinned .NET runtime (hostfxr status 0x%x)\n",
        status);
    return 1;
  }
  void* pointer = nullptr;
  const int delegate_status = get_delegate(
      handle, hdt_load_assembly_and_get_function_pointer, &pointer);
  close(handle);  // Closes configuration only, not the process-owned runtime.
  if (delegate_status != 0 || !pointer) return 1;
  g_load = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(pointer);
  return 0;
}

static auto load_card() -> int {
  const auto load = g_load;
  void* check = nullptr;
  if (!load_entry(load, "CheckLayout", &check) ||
      reinterpret_cast<CheckLayout>(check)(
          k_layout, sizeof k_layout / sizeof k_layout[0]) != 0) {
    std::fprintf(stderr,
                 "The C# SDK does not match this worker's Card ABI "
                 "(tsugumi/sdk/abi.h); see above\n");
    return 1;
  }
  return load_entry(load, "Execute", reinterpret_cast<void**>(&g_main)) ? 0 : 1;
}

extern "C" auto tsugumi_card_invoke(const tsugumi_call* call) -> int {
  switch (call->phase) {
    case TSUGUMI_PREPARE: return prepare();
    case TSUGUMI_LOAD: return load_card();
    case TSUGUMI_RUN_START:
    case TSUGUMI_RUN:
      return g_main(call);
  }
  return 1;
}
