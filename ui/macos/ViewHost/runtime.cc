#include <time.h>
#include <tsugumi/tsugumi_view.h>
#include <wasmtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "tsugumi_view_host.h"
#include "webgpu_imports.gen.h"

struct tsugumi_view_stop {
  std::atomic<bool> requested{false};
};

namespace {

using tsugumi::view::Context;
using tsugumi::view::Handles;
using tsugumi::view::Import;
using tsugumi::view::Kind;
using tsugumi::view::Memory;
using tsugumi::view::Procs;
using tsugumi::view::TrapError;
using tsugumi::view::Val;
using tsugumi::view::ValType;

template <auto Destroy>
struct Deleter {
  auto operator()(auto* value) const -> void { Destroy(value); }
};

template <typename T, auto Destroy>
using Owned = std::unique_ptr<T, Deleter<Destroy>>;

using SurfaceTexture = Owned<WGPUTextureImpl, wgpuTextureRelease>;
using TextureView = Owned<WGPUTextureViewImpl, wgpuTextureViewRelease>;

constexpr auto kFormat = WGPUTextureFormat_BGRA8Unorm;

auto copy_error(char* error, size_t size, std::string_view message) -> void {
  if (error && size) {
    const auto length = std::min(size - 1, message.size());
    if (length) std::memcpy(error, message.data(), length);
    error[length] = '\0';
  }
}

auto check_wasm(wasmtime_error_t* error, wasm_trap_t* trap,
                std::string_view operation) -> void {
  if (!error && !trap) return;
  Owned<wasmtime_error_t, wasmtime_error_delete> owned_error(error);
  Owned<wasm_trap_t, wasm_trap_delete> owned_trap(trap);
  wasm_byte_vec_t message{};
  if (error) {
    wasmtime_error_message(error, &message);
  } else {
    wasm_trap_message(trap, &message);
  }
  const std::unique_ptr<wasm_byte_vec_t, Deleter<wasm_byte_vec_delete>>
      owned_message(&message);
  std::string_view detail(message.data, message.size);
  if (!detail.empty() && detail.back() == '\0') detail.remove_suffix(1);
  throw TrapError(std::string(operation) + ": " + std::string(detail));
}

auto i32(uint32_t value) -> wasmtime_val_t {
  return {.kind = WASMTIME_I32, .of = {.i32 = static_cast<int32_t>(value)}};
}

auto i64(uint64_t value) -> wasmtime_val_t {
  return {.kind = WASMTIME_I64, .of = {.i64 = static_cast<int64_t>(value)}};
}

auto uptime() -> double {
  return static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1e9;
}

auto f32(float value) -> wasmtime_val_t {
  return {.kind = WASMTIME_F32, .of = {.f32 = value}};
}

constexpr std::array kWasmKinds{WASM_I32, WASM_I64, WASM_F32, WASM_F64};
constexpr std::array<wasmtime_valkind_t, 4> kWasmtimeKinds{
    WASMTIME_I32, WASMTIME_I64, WASMTIME_F32, WASMTIME_F64};

auto wasm_types(std::span<const ValType> kinds) -> wasm_valtype_vec_t {
  wasm_valtype_vec_t types{};
  wasm_valtype_vec_new_uninitialized(&types, kinds.size());
  for (size_t i = 0; i < kinds.size(); ++i) {
    types.data[i] = wasm_valtype_new(kWasmKinds[static_cast<size_t>(kinds[i])]);
  }
  return types;
}

auto matches(const wasm_valtype_vec_t* types, std::span<const ValType> kinds)
    -> bool {
  if (types->size != kinds.size()) return false;
  for (size_t i = 0; i < kinds.size(); ++i) {
    if (wasm_valtype_kind(types->data[i]) !=
        kWasmKinds[static_cast<size_t>(kinds[i])]) {
      return false;
    }
  }
  return true;
}

auto decode_value(const wasmtime_val_t& arg) -> Val {
  switch (arg.kind) {
    case WASMTIME_I32:
      return {.i32 = arg.of.i32};
    case WASMTIME_I64:
      return {.i64 = arg.of.i64};
    case WASMTIME_F32:
      return {.f32 = arg.of.f32};
    case WASMTIME_F64:
      return {.f64 = arg.of.f64};
    default:
      throw TrapError("Unexpected WebGPU argument type");
  }
}

auto encode_value(ValType type, Val value) -> wasmtime_val_t {
  wasmtime_val_t result{.kind = kWasmtimeKinds[static_cast<size_t>(type)],
                        .of = {}};
  switch (type) {
    case ValType::I32:
      result.of.i32 = value.i32;
      break;
    case ValType::I64:
      result.of.i64 = value.i64;
      break;
    case ValType::F32:
      result.of.f32 = value.f32;
      break;
    case ValType::F64:
      result.of.f64 = value.f64;
      break;
  }
  return result;
}

auto caller_memory(wasmtime_caller_t* caller) -> Memory {
  wasmtime_extern_t memory{};
  if (!wasmtime_caller_export_get(caller, "memory", 6, &memory)) {
    throw TrapError("Missing memory export");
  }
  const std::unique_ptr<wasmtime_extern_t, Deleter<wasmtime_extern_delete>>
      owned_memory(&memory);
  if (memory.kind == WASMTIME_EXTERN_SHAREDMEMORY) {
    return {wasmtime_sharedmemory_data(memory.of.sharedmemory),
            wasmtime_sharedmemory_data_size(memory.of.sharedmemory)};
  }
  if (memory.kind != WASMTIME_EXTERN_MEMORY)
    throw TrapError("Expected a memory export");
  auto* context = wasmtime_caller_context(caller);
  return {wasmtime_memory_data(context, &memory.of.memory),
          wasmtime_memory_data_size(context, &memory.of.memory)};
}

auto in_bounds(Memory memory, uint32_t offset, uint32_t size) -> bool {
  return offset <= memory.size && size <= memory.size - offset;
}

auto dawn_message(WGPUStringView message) -> std::string_view {
  if (!message.data) return {};
  if (message.length == WGPU_STRLEN) return message.data;
  return {message.data, message.length};
}

auto wait_for(WGPUInstance instance, WGPUFuture future) -> void {
  constexpr uint64_t kWaitNanoseconds = 1'000'000'000;
  WGPUFutureWaitInfo wait{future, false};
  WGPUWaitStatus status;
  do {
    status = wgpuInstanceWaitAny(instance, 1, &wait, kWaitNanoseconds);
  } while (status == WGPUWaitStatus_TimedOut);
  if (status != WGPUWaitStatus_Success) {
    throw TrapError("Dawn could not wait for the request");
  }
}

struct Export {
  const char* name;
  std::initializer_list<ValType> params;
};

using enum ValType;
constexpr std::array kResult{I32};
constexpr std::array kReadParams{I32, I32};
constexpr std::array kSendParams{I32, I32, I32};
const Export kAbi{"tsugumi_view_abi", {}};
const Export kStart{"tsugumi_view_start", {I32, I32, I32, I32, I32, F32}};
const Export kResize{"tsugumi_view_resize", {I32, I32, F32, I64}};
const Export kDraw{"tsugumi_view_draw", {I32, I64}};
const Export kMessage{"tsugumi_view_message", {I32, I64}};
const Export kPointer{"tsugumi_view_pointer",
                      {I32, F32, F32, I32, F32, F32, I32, I64}};
const Export kKey{"tsugumi_view_key", {I32, I32, I32, I64}};
const Export kAudio{"tsugumi_view_audio", {I32, I64}};
const Export kTextInput{"tsugumi_view_text_input", {}};
const Export kText{"tsugumi_view_text", {I32, I64}};
const Export kComposition{"tsugumi_view_composition", {I32, I32, I64}};
const Export kGamepad{"tsugumi_view_gamepad",
                      {I32, I32, I32, F32, F32, F32, F32, F32, F32, I64}};

auto make_engine() -> wasm_engine_t* {
  auto* config = wasm_config_new();
  if (!config) return nullptr;
  wasmtime_config_epoch_interruption_set(config, true);
  return wasm_engine_new_with_config(config);
}

class EpochTicker {
 public:
  explicit EpochTicker(wasm_engine_t* engine)
      : thread_([this, engine] {
          using namespace std::chrono_literals;
          std::unique_lock lock(mutex_);
          while (!wakeup_.wait_for(lock, 20ms, [this] { return done_; }))
            wasmtime_engine_increment_epoch(engine);
        }) {}
  ~EpochTicker() {
    {
      const std::lock_guard lock(mutex_);
      done_ = true;
    }
    wakeup_.notify_one();
    thread_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable wakeup_;
  bool done_ = false;
  std::thread thread_;
};

struct Gpu {
  mutable std::mutex error_mutex;
  std::string error;
  Owned<WGPUInstanceImpl, wgpuInstanceRelease> instance;
  Owned<WGPUSurfaceImpl, wgpuSurfaceRelease> surface;
  Owned<WGPUAdapterImpl, wgpuAdapterRelease> adapter;
  Owned<WGPUDeviceImpl, wgpuDeviceRelease> device;
  Owned<WGPUQueueImpl, wgpuQueueRelease> queue;
  bool configured = false;

  ~Gpu() { close(); }

  auto close() -> void {
    if (configured) {
      wgpuSurfaceUnconfigure(surface.get());
      configured = false;
    }
    if (device) wgpuDeviceDestroy(device.get());
    queue.reset();
    device.reset();
    adapter.reset();
    surface.reset();
    process_events();
    instance.reset();
  }

  auto process_events() const -> void {
    if (instance) wgpuInstanceProcessEvents(instance.get());
  }

  auto record_error(std::string_view prefix, WGPUStringView message) noexcept
      -> void {
    try {
      // Dawn's uncaptured-error callback can be spontaneous.
      const std::lock_guard lock(error_mutex);
      if (error.empty())
        error = std::string(prefix).append(dawn_message(message));
    } catch (const std::exception&) {
      // Never unwind across a Dawn callback.
    }
  }

  auto first_error() const -> const char* {
    const std::lock_guard lock(error_mutex);
    return error.empty() ? nullptr : error.c_str();
  }

  auto open(void* layer) -> void {
    const auto feature = WGPUInstanceFeatureName_TimedWaitAny;
    WGPUInstanceDescriptor instance_desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    instance_desc.requiredFeatureCount = 1;
    instance_desc.requiredFeatures = &feature;
    instance.reset(wgpuCreateInstance(&instance_desc));
    if (!instance) throw TrapError("Could not create Dawn instance");

    WGPUSurfaceSourceMetalLayer source = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
    source.layer = layer;
    WGPUSurfaceDescriptor surface_desc = WGPU_SURFACE_DESCRIPTOR_INIT;
    surface_desc.nextInChain = &source.chain;
    surface.reset(wgpuInstanceCreateSurface(instance.get(), &surface_desc));
    if (!surface) throw TrapError("Could not create Dawn surface");

    // WaitAnyOnly runs the request callbacks on the calling thread.
    WGPURequestAdapterOptions opts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    opts.backendType = WGPUBackendType_Metal;
    opts.compatibleSurface = surface.get();
    WGPURequestAdapterCallbackInfo adapter_cb =
        WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    adapter_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    adapter_cb.userdata1 = this;
    adapter_cb.callback = [](WGPURequestAdapterStatus status,
                             WGPUAdapter adapter, WGPUStringView message,
                             void* data, void*) {
      auto& gpu = *static_cast<Gpu*>(data);
      gpu.adapter.reset(adapter);
      if (status != WGPURequestAdapterStatus_Success) {
        gpu.record_error("Dawn adapter request: ", message);
      }
    };
    wait_for(instance.get(),
             wgpuInstanceRequestAdapter(instance.get(), &opts, adapter_cb));
    if (!adapter) {
      const auto* failure = first_error();
      throw TrapError(failure ? failure : "No Metal adapter");
    }

    WGPUDeviceDescriptor device_desc = WGPU_DEVICE_DESCRIPTOR_INIT;
    device_desc.deviceLostCallbackInfo.mode =
        WGPUCallbackMode_AllowProcessEvents;
    device_desc.deviceLostCallbackInfo.userdata1 = this;
    device_desc.deviceLostCallbackInfo.callback =
        [](const WGPUDevice*, WGPUDeviceLostReason reason,
           WGPUStringView message, void* data, void*) {
          if (reason != WGPUDeviceLostReason_Destroyed) {
            static_cast<Gpu*>(data)->record_error("WebGPU device lost: ",
                                                  message);
          }
        };
    device_desc.uncapturedErrorCallbackInfo.userdata1 = this;
    device_desc.uncapturedErrorCallbackInfo.callback =
        [](const WGPUDevice*, WGPUErrorType, WGPUStringView message, void* data,
           void*) {
          static_cast<Gpu*>(data)->record_error("WebGPU: ", message);
        };
    WGPURequestDeviceCallbackInfo device_cb =
        WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    device_cb.mode = WGPUCallbackMode_WaitAnyOnly;
    device_cb.userdata1 = this;
    device_cb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device,
                            WGPUStringView message, void* data, void*) {
      auto& gpu = *static_cast<Gpu*>(data);
      gpu.device.reset(device);
      if (status != WGPURequestDeviceStatus_Success) {
        gpu.record_error("Dawn device request: ", message);
      }
    };
    wait_for(instance.get(),
             wgpuAdapterRequestDevice(adapter.get(), &device_desc, device_cb));
    if (!device) {
      const auto* error = first_error();
      throw TrapError(error ? error : "No Dawn device");
    }
    queue.reset(wgpuDeviceGetQueue(device.get()));
    if (!queue) throw TrapError("Could not get the Dawn queue");
  }

  auto configure(uint32_t width, uint32_t height) -> void {
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = device.get();
    config.format = kFormat;
    config.width = width;
    config.height = height;
    config.alphaMode = WGPUCompositeAlphaMode_Opaque;
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface.get(), &config);
    configured = true;
  }
};

}  // namespace

struct tsugumi_view_host {
  Gpu gpu;
  Handles handles;
  Procs procs = tsugumi::view::dawnProcs();
  Owned<wasm_engine_t, wasm_engine_delete> engine{make_engine()};
  std::optional<EpochTicker> ticker;
  Owned<wasmtime_store_t, wasmtime_store_delete> store;
  wasmtime_context_t* context = nullptr;
  wasmtime_instance_t instance{};
  wasmtime_func_t abi{}, start{}, resize{}, draw{};
  std::optional<wasmtime_func_t> message, pointer, key, audio, text_input, text,
      composition, gamepad;
  std::optional<tsugumi_view_host_text_area> text_area;
  tsugumi_view_stop* stop = nullptr;
  tsugumi_view_host_waiting_fn waiting = nullptr;
  bool is_waiting = false;
  double call_started = 0;
  double started = 0;
  double last_present = 0;
  uint32_t width = 0, height = 0;
  std::optional<std::span<const uint8_t>> current_message;
  tsugumi_view_host_send_fn send = nullptr;
  void* send_context = nullptr;
  std::string dropped;

  ~tsugumi_view_host();

  auto release_handles() -> void;

  auto load(const uint8_t* wasm, size_t size) -> void;
  auto check_abi() -> void;
  auto begin_call() -> void;
  auto end_call() -> void;
  auto read_memory(uint32_t offset, void* data, uint32_t size) -> void;
  auto render(WGPUTexture texture, double present) -> int32_t;
  auto view_time(double at) const -> wasmtime_val_t {
    return i64(static_cast<uint64_t>(std::max(0.0, at - started) *
                                     TSUGUMI_VIEW_SAMPLE_RATE));
  }
  auto incoming(const uint8_t* bytes, uint32_t size, const Export& spec,
                const std::optional<wasmtime_func_t>& func,
                std::initializer_list<wasmtime_val_t> args) -> int32_t {
    if (!func) return 0;
    current_message.emplace(bytes, size);
    struct ClearMessage {
      tsugumi_view_host& host;
      ~ClearMessage() { host.current_message.reset(); }
    } clear{*this};
    return call(spec.name, *func, args);
  }
  auto call(const char* name, const wasmtime_func_t& func,
            std::initializer_list<wasmtime_val_t> args,
            bool pointer_result = false) -> int32_t;

  template <typename Operation>
  auto run(Operation operation) -> int32_t {
    if (!dropped.empty()) return -1;
    try {
      const auto result = operation();
      gpu.process_events();
      return result;
    } catch (const std::exception& error) {
      dropped = error.what();
      gpu.process_events();
      store.reset();
      release_handles();
      gpu.close();
      return -1;
    }
  }

  auto export_func(const Export& spec, bool required = true)
      -> std::optional<wasmtime_func_t>;
};

namespace {

auto caller_host(wasmtime_caller_t* caller) -> tsugumi_view_host& {
  return *static_cast<tsugumi_view_host*>(
      wasmtime_context_get_data(wasmtime_caller_context(caller)));
}

auto gpu_import(void* data, wasmtime_caller_t* caller,
                const wasmtime_val_t* args, size_t nargs,
                wasmtime_val_t* results, size_t nresults) -> wasm_trap_t* {
  try {
    const auto& import = *static_cast<const Import*>(data);
    auto& host = caller_host(caller);
    Context context{caller_memory(caller), host.handles, host.procs};
    std::vector<Val> params;
    params.reserve(nargs);
    for (size_t i = 0; i < nargs; ++i) params.push_back(decode_value(args[i]));
    std::vector<Val> values(nresults);
    import.call(context, params.data(), values.data());
    for (size_t i = 0; i < nresults; ++i)
      results[i] = encode_value(import.results[i], values[i]);
    return nullptr;
  } catch (const std::exception& error) {
    return wasmtime_trap_new(error.what(), std::strlen(error.what()));
  }
}

template <bool Read>
auto message_import(void*, wasmtime_caller_t* caller,
                    const wasmtime_val_t* args, size_t, wasmtime_val_t* results,
                    size_t) -> wasm_trap_t* {
  try {
    auto& host = caller_host(caller);
    const auto offset = static_cast<uint32_t>(args[Read ? 0 : 1].of.i32);
    const auto size = static_cast<uint32_t>(args[Read ? 1 : 2].of.i32);
    const auto memory = caller_memory(caller);
    results[0] = i32(-1);
    if (!in_bounds(memory, offset, size)) return nullptr;
    if constexpr (Read) {
      if (host.current_message && size == host.current_message->size()) {
        if (size)
          std::memcpy(memory.base + offset, host.current_message->data(), size);
        results[0] = i32(0);
      }
    } else if (size <= TSUGUMI_MESSAGE_MAX && host.send) {
      const auto route = static_cast<uint32_t>(args[0].of.i32);
      if (!in_bounds(memory, route, 16)) return nullptr;
      const auto* bytes = offset ? memory.base + offset : memory.base;
      results[0] =
          i32(host.send(host.send_context, memory.base + route, bytes, size));
    }
    return nullptr;
  } catch (const std::exception& error) {
    return wasmtime_trap_new(error.what(), std::strlen(error.what()));
  }
}

auto define_import(wasmtime_linker_t* linker, std::string_view module,
                   std::string_view name, std::span<const ValType> params,
                   std::span<const ValType> results,
                   wasmtime_func_callback_t callback, void* data = nullptr)
    -> void {
  auto param_types = wasm_types(params);
  auto result_types = wasm_types(results);
  Owned<wasm_functype_t, wasm_functype_delete> type(
      wasm_functype_new(&param_types, &result_types));
  check_wasm(wasmtime_linker_define_func(linker, module.data(), module.size(),
                                         name.data(), name.size(), type.get(),
                                         callback, data, nullptr),
             nullptr, name);
}

}  // namespace

auto tsugumi_view_host::export_func(const Export& spec, bool required)
    -> std::optional<wasmtime_func_t> {
  wasmtime_extern_t ext{};
  if (!wasmtime_instance_export_get(context, &instance, spec.name,
                                    std::strlen(spec.name), &ext)) {
    if (required) throw TrapError(std::string("Missing export ") + spec.name);
    return std::nullopt;
  }
  const std::unique_ptr<wasmtime_extern_t, Deleter<wasmtime_extern_delete>>
      owned_export(&ext);
  if (ext.kind == WASMTIME_EXTERN_FUNC) {
    Owned<wasm_functype_t, wasm_functype_delete> type(
        wasmtime_func_type(context, &ext.of.func));
    if (matches(wasm_functype_params(type.get()), spec.params) &&
        matches(wasm_functype_results(type.get()), kResult)) {
      return ext.of.func;
    }
  }
  throw TrapError(std::string("Invalid signature for export ") + spec.name);
}

auto tsugumi_view_host::load(const uint8_t* wasm, size_t size) -> void {
  if (!engine) throw TrapError("Could not create Wasmtime engine");
  ticker.emplace(engine.get());
  wasmtime_module_t* compiled = nullptr;
  check_wasm(wasmtime_module_new(engine.get(), wasm, size, &compiled), nullptr,
             "Compile view");
  Owned<wasmtime_module_t, wasmtime_module_delete> module(compiled);
  Owned<wasmtime_linker_t, wasmtime_linker_delete> linker(
      wasmtime_linker_new(engine.get()));

  // Link only the ABI's functions. Wasmtime rejects any unresolved import or
  // an import whose kind or signature differs from its definition.
  define_import(linker.get(), "tsugumi", "tsugumi_view_read", kReadParams,
                kResult, message_import<true>);
  define_import(linker.get(), "tsugumi", "tsugumi_view_send", kSendParams,
                kResult, message_import<false>);
  for (const auto& import : tsugumi::view::imports()) {
    define_import(linker.get(), "webgpu", import.name, import.params,
                  import.results, gpu_import, const_cast<Import*>(&import));
  }

  store.reset(wasmtime_store_new(engine.get(), this, nullptr));
  context = wasmtime_store_context(store.get());
  wasmtime_store_epoch_deadline_callback(
      store.get(),
      [](const wasmtime_context_t*, void* data, uint64_t* delta,
         wasmtime_update_deadline_kind_t* kind) -> wasmtime_error_t* {
        auto& host = *static_cast<tsugumi_view_host*>(data);
        if (host.stop && host.stop->requested.load(std::memory_order_relaxed))
          return wasmtime_error_new("The view was stopped");
        if (!host.is_waiting && uptime() - host.call_started > 0.5) {
          host.is_waiting = true;
          if (host.waiting) host.waiting(host.send_context, 1);
        }
        *delta = 1;
        *kind = WASMTIME_UPDATE_DEADLINE_CONTINUE;
        return nullptr;
      },
      this, nullptr);
  begin_call();
  wasm_trap_t* trap = nullptr;
  auto* error = wasmtime_linker_instantiate(linker.get(), context, module.get(),
                                            &instance, &trap);
  end_call();
  check_wasm(error, trap, "Instantiate view");

  wasmtime_extern_t memory{};
  if (!wasmtime_instance_export_get(context, &instance, "memory", 6, &memory)) {
    throw TrapError("Missing memory export");
  }
  const std::unique_ptr<wasmtime_extern_t, Deleter<wasmtime_extern_delete>>
      owned_memory(&memory);
  const Owned<wasm_externtype_t, wasm_externtype_delete> type(
      wasmtime_extern_type(context, &memory));
  const auto* memory_type = wasm_externtype_as_memorytype_const(type.get());
  if (!memory_type || wasmtime_memorytype_is64(memory_type)) {
    throw TrapError("Expected a wasm32 memory export");
  }
  abi = *export_func(kAbi);
  start = *export_func(kStart);
  resize = *export_func(kResize);
  draw = *export_func(kDraw);
  message = export_func(kMessage, false);
  pointer = export_func(kPointer, false);
  key = export_func(kKey, false);
  audio = export_func(kAudio, false);
  text_input = export_func(kTextInput, false);
  text = export_func(kText, false);
  composition = export_func(kComposition, false);
  gamepad = export_func(kGamepad, false);
}

auto tsugumi_view_host::check_abi() -> void {
  const auto version = call(kAbi.name, abi, {});
  if (version != TSUGUMI_VIEW_ABI) {
    throw TrapError("Unsupported view ABI " + std::to_string(version));
  }
}

auto tsugumi_view_host::begin_call() -> void {
  if (stop && stop->requested.load(std::memory_order_relaxed))
    throw TrapError("The view was stopped");
  call_started = uptime();
  wasmtime_context_set_epoch_deadline(context, 1);
}

auto tsugumi_view_host::end_call() -> void {
  if (is_waiting) {
    is_waiting = false;
    if (waiting) waiting(send_context, 0);
  }
}

auto tsugumi_view_host::read_memory(uint32_t offset, void* data, uint32_t size)
    -> void {
  wasmtime_extern_t ext{};
  if (!wasmtime_instance_export_get(context, &instance, "memory", 6, &ext))
    throw TrapError("Missing memory export");
  const std::unique_ptr<wasmtime_extern_t, Deleter<wasmtime_extern_delete>>
      owned(&ext);
  const auto memory =
      ext.kind == WASMTIME_EXTERN_SHAREDMEMORY
          ? Memory{wasmtime_sharedmemory_data(ext.of.sharedmemory),
                   wasmtime_sharedmemory_data_size(ext.of.sharedmemory)}
          : Memory{wasmtime_memory_data(context, &ext.of.memory),
                   wasmtime_memory_data_size(context, &ext.of.memory)};
  if (!in_bounds(memory, offset, size))
    throw TrapError("View returned an out-of-bounds pointer");
  std::memcpy(data, memory.base + offset, size);
}

auto tsugumi_view_host::call(const char* name, const wasmtime_func_t& func,
                             std::initializer_list<wasmtime_val_t> args,
                             bool pointer_result) -> int32_t {
  begin_call();
  wasmtime_val_t result{};
  wasm_trap_t* trap = nullptr;
  auto* error = wasmtime_func_call(context, &func, args.begin(), args.size(),
                                   &result, 1, &trap);
  end_call();
  check_wasm(error, trap, name);
  if (!pointer_result && result.of.i32 < 0) {
    throw TrapError(std::string(name) + " returned " +
                    std::to_string(result.of.i32));
  }
  return result.of.i32;
}

auto tsugumi_view_host::release_handles() -> void {
  for (const auto& [id, entry] : handles.drain()) {
    switch (entry.kind) {
      // Device and queue are owned by Gpu.
      case Kind::Device:
      case Kind::Queue:
        break;
      case Kind::BindGroup:
        wgpuBindGroupRelease(static_cast<WGPUBindGroup>(entry.native));
        break;
      case Kind::BindGroupLayout:
        wgpuBindGroupLayoutRelease(
            static_cast<WGPUBindGroupLayout>(entry.native));
        break;
      case Kind::Buffer:
        wgpuBufferRelease(static_cast<WGPUBuffer>(entry.native));
        break;
      case Kind::CommandBuffer:
        wgpuCommandBufferRelease(static_cast<WGPUCommandBuffer>(entry.native));
        break;
      case Kind::CommandEncoder:
        wgpuCommandEncoderRelease(
            static_cast<WGPUCommandEncoder>(entry.native));
        break;
      case Kind::ComputePassEncoder:
        wgpuComputePassEncoderRelease(
            static_cast<WGPUComputePassEncoder>(entry.native));
        break;
      case Kind::ComputePipeline:
        wgpuComputePipelineRelease(
            static_cast<WGPUComputePipeline>(entry.native));
        break;
      case Kind::PipelineLayout:
        wgpuPipelineLayoutRelease(
            static_cast<WGPUPipelineLayout>(entry.native));
        break;
      case Kind::QuerySet:
        wgpuQuerySetRelease(static_cast<WGPUQuerySet>(entry.native));
        break;
      case Kind::RenderPassEncoder:
        wgpuRenderPassEncoderRelease(
            static_cast<WGPURenderPassEncoder>(entry.native));
        break;
      case Kind::RenderPipeline:
        wgpuRenderPipelineRelease(
            static_cast<WGPURenderPipeline>(entry.native));
        break;
      case Kind::Sampler:
        wgpuSamplerRelease(static_cast<WGPUSampler>(entry.native));
        break;
      case Kind::ShaderModule:
        wgpuShaderModuleRelease(static_cast<WGPUShaderModule>(entry.native));
        break;
      case Kind::Texture:
        wgpuTextureRelease(static_cast<WGPUTexture>(entry.native));
        break;
      case Kind::TextureView:
        wgpuTextureViewRelease(static_cast<WGPUTextureView>(entry.native));
        break;
    }
  }
}

tsugumi_view_host::~tsugumi_view_host() { release_handles(); }

auto tsugumi_view_stop_create() -> tsugumi_view_stop* {
  return new (std::nothrow) tsugumi_view_stop;
}

auto tsugumi_view_stop_request(tsugumi_view_stop* stop) -> void {
  stop->requested.store(true, std::memory_order_relaxed);
}

auto tsugumi_view_stop_destroy(tsugumi_view_stop* stop) -> void { delete stop; }

auto tsugumi_view_host_check(const uint8_t* wasm, size_t size, char* error,
                             size_t error_size) -> int32_t {
  try {
    tsugumi_view_host host;
    host.load(wasm, size);
    host.check_abi();
    copy_error(error, error_size, "");
    return 0;
  } catch (const std::exception& failure) {
    copy_error(error, error_size, failure.what());
    return -1;
  }
}

auto tsugumi_view_host_create(
    const uint8_t* wasm, size_t size, void* metal_layer, uint32_t width,
    uint32_t height, float scale, tsugumi_view_stop* stop,
    tsugumi_view_host_send_fn send, tsugumi_view_host_waiting_fn waiting,
    void* send_context, char* error, size_t error_size) -> tsugumi_view_host* {
  try {
    auto host = std::make_unique<tsugumi_view_host>();
    host->stop = stop;
    host->send = send;
    host->waiting = waiting;
    host->send_context = send_context;
    host->load(wasm, size);
    host->gpu.open(metal_layer);
    host->gpu.configure(width, height);
    host->gpu.process_events();
    if (const auto* failure = host->gpu.first_error()) throw TrapError(failure);
    host->check_abi();
    host->gpu.process_events();
    const auto device = host->handles.add(Kind::Device, host->gpu.device.get());
    const auto queue = host->handles.add(Kind::Queue, host->gpu.queue.get());
    host->width = width;
    host->height = height;
    host->started = uptime();
    host->call(kStart.name, host->start,
               {i32(device), i32(queue), i32(kFormat), i32(width), i32(height),
                f32(scale)});
    host->gpu.process_events();
    copy_error(error, error_size, "");
    return host.release();
  } catch (const std::exception& failure) {
    copy_error(error, error_size, failure.what());
    return nullptr;
  }
}

auto tsugumi_view_host::render(WGPUTexture texture, double present) -> int32_t {
  last_present = std::max(last_present, present);
  TextureView view(wgpuTextureCreateView(texture, nullptr));
  if (!view) throw TrapError("Could not create the Dawn render target view");
  const auto id = handles.add(Kind::TextureView, view.get());
  view.release();
  // The module may also release this id inside draw().
  struct FrameView {
    Handles& handles;
    uint32_t id;
    ~FrameView() {
      TextureView remaining(static_cast<WGPUTextureView>(
          handles.takeIfPresent(Kind::TextureView, id)));
    }
  };
  const auto flags = [&] {
    FrameView frame{handles, id};
    return call(kDraw.name, draw, {i32(id), view_time(last_present)});
  }();
  text_area.reset();
  if (text_input) {
    const auto offset =
        static_cast<uint32_t>(call(kTextInput.name, *text_input, {}, true));
    if (offset) {
      tsugumi_view_host_text_area area{};
      read_memory(offset, &area, sizeof(area));
      if (std::isfinite(area.x) && std::isfinite(area.y) &&
          std::isfinite(area.width) && std::isfinite(area.height) &&
          area.width >= 0 && area.height >= 0)
        text_area = area;
    }
  }
  return flags;
}

auto tsugumi_view_host_draw(tsugumi_view_host* host, double present)
    -> int32_t {
  return host->run([&] {
    WGPUSurfaceTexture acquired = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(host->gpu.surface.get(), &acquired);
    SurfaceTexture texture(acquired.texture);
    if (!texture ||
        (acquired.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
         acquired.status !=
             WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)) {
      throw TrapError("Could not acquire the Dawn surface texture (status " +
                      std::to_string(acquired.status) + ")");
    }

    const auto flags = host->render(texture.get(), present);
    if (wgpuSurfacePresent(host->gpu.surface.get()) != WGPUStatus_Success) {
      throw TrapError("Could not present the Dawn surface");
    }
    return flags;
  });
}

auto tsugumi_view_host_snapshot(tsugumi_view_host* host, double present,
                                tsugumi_view_host_pixels_fn pixels,
                                void* context) -> int32_t {
  constexpr uint64_t kMaxBytes = 64u * 1024u * 1024u;
  const uint64_t stride = (uint64_t{host->width} * 4 + 255) & ~255ull;
  const uint32_t height = host->height;
  if (!host->gpu.device || !stride || !height || stride > kMaxBytes / height)
    return -2;
  const auto size = static_cast<size_t>(stride * height);
  WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
  desc.format = kFormat;
  desc.size = {host->width, height, 1};
  desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
  SurfaceTexture target(wgpuDeviceCreateTexture(host->gpu.device.get(), &desc));
  if (!target) return -2;
  if (host->run([&] { return host->render(target.get(), present); }) < 0)
    return -1;
  try {
    WGPUBufferDescriptor readback = WGPU_BUFFER_DESCRIPTOR_INIT;
    readback.size = size;
    readback.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    Owned<WGPUBufferImpl, wgpuBufferRelease> buffer(
        wgpuDeviceCreateBuffer(host->gpu.device.get(), &readback));
    Owned<WGPUCommandEncoderImpl, wgpuCommandEncoderRelease> encoder(
        wgpuDeviceCreateCommandEncoder(host->gpu.device.get(), nullptr));
    if (!buffer || !encoder) return -2;
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = target.get();
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buffer.get();
    dst.layout.bytesPerRow = static_cast<uint32_t>(stride);
    dst.layout.rowsPerImage = height;
    wgpuCommandEncoderCopyTextureToBuffer(encoder.get(), &src, &dst,
                                          &desc.size);
    Owned<WGPUCommandBufferImpl, wgpuCommandBufferRelease> commands(
        wgpuCommandEncoderFinish(encoder.get(), nullptr));
    if (!commands) return -2;
    const auto command = commands.get();
    wgpuQueueSubmit(host->gpu.queue.get(), 1, &command);
    bool mapped = false;
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.userdata1 = &mapped;
    callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView,
                           void* data, void*) {
      *static_cast<bool*>(data) = status == WGPUMapAsyncStatus_Success;
    };
    wait_for(
        host->gpu.instance.get(),
        wgpuBufferMapAsync(buffer.get(), WGPUMapMode_Read, 0, size, callback));
    if (!mapped) return -2;
    const auto* data = static_cast<const uint8_t*>(
        wgpuBufferGetConstMappedRange(buffer.get(), 0, size));
    if (data)
      pixels(context, data, host->width, height, static_cast<uint32_t>(stride));
    wgpuBufferUnmap(buffer.get());
    return data ? 0 : -2;
  } catch (const std::exception&) {
    return -2;
  }
}

auto tsugumi_view_host_resize(tsugumi_view_host* host, uint32_t width,
                              uint32_t height, float scale, double at)
    -> int32_t {
  return host->run([&] {
    host->gpu.configure(width, height);
    host->width = width;
    host->height = height;
    return host->call(
        kResize.name, host->resize,
        {i32(width), i32(height), f32(scale), host->view_time(at)});
  });
}

auto tsugumi_view_host_message(tsugumi_view_host* host, const uint8_t* bytes,
                               uint32_t size, double at) -> int32_t {
  return host->run([&] {
    return host->incoming(bytes, size, kMessage, host->message,
                          {i32(size), host->view_time(at)});
  });
}

auto tsugumi_view_host_pointer(tsugumi_view_host* host, uint32_t kind, float x,
                               float y, uint32_t buttons, float wheel_x,
                               float wheel_y, uint32_t modifiers, double at)
    -> int32_t {
  return host->run([&] {
    if (!host->pointer) return int32_t{0};
    return host->call(kPointer.name, *host->pointer,
                      {i32(kind), f32(x), f32(y), i32(buttons), f32(wheel_x),
                       f32(wheel_y), i32(modifiers), host->view_time(at)});
  });
}

auto tsugumi_view_host_key(tsugumi_view_host* host, uint32_t kind,
                           uint32_t code, uint32_t modifiers, double at)
    -> int32_t {
  return host->run([&] {
    if (!host->key) return int32_t{0};
    return host->call(
        kKey.name, *host->key,
        {i32(kind), i32(code), i32(modifiers), host->view_time(at)});
  });
}

auto tsugumi_view_host_text(tsugumi_view_host* host, const uint8_t* bytes,
                            uint32_t size, double at) -> int32_t {
  return host->run([&] {
    return host->incoming(bytes, size, kText, host->text,
                          {i32(size), host->view_time(at)});
  });
}

auto tsugumi_view_host_composition(tsugumi_view_host* host,
                                   const uint8_t* bytes, uint32_t size,
                                   uint32_t cursor, double at) -> int32_t {
  return host->run([&] {
    return host->incoming(bytes, size, kComposition, host->composition,
                          {i32(size), i32(cursor), host->view_time(at)});
  });
}

auto tsugumi_view_host_text_input(const tsugumi_view_host* host,
                                  tsugumi_view_host_text_area* area)
    -> int32_t {
  if (!host->text_area) return 0;
  *area = *host->text_area;
  return 1;
}

auto tsugumi_view_host_gamepad(tsugumi_view_host* host, uint32_t pad,
                               uint32_t kind, uint32_t buttons, float lx,
                               float ly, float rx, float ry, float lt, float rt,
                               double at) -> int32_t {
  return host->run([&] {
    if (!host->gamepad) return int32_t{0};
    return host->call(
        kGamepad.name, *host->gamepad,
        {i32(pad), i32(kind), i32(buttons), f32(lx), f32(ly), f32(rx), f32(ry),
         f32(lt), f32(rt), host->view_time(at)});
  });
}

auto tsugumi_view_host_audio(tsugumi_view_host* host, uint32_t frames,
                             double heard, float* output) -> int32_t {
  return host->run([&] {
    if (!host->audio || frames > TSUGUMI_VIEW_SAMPLE_RATE) return int32_t{-1};
    const auto offset = static_cast<uint32_t>(
        host->call(kAudio.name, *host->audio,
                   {i32(frames), host->view_time(heard)}, true));
    if (offset)
      host->read_memory(offset, output, frames * 2 * sizeof(float));
    else
      std::fill_n(output, frames * 2, 0.0f);
    return int32_t{0};
  });
}

auto tsugumi_view_host_accepts(const tsugumi_view_host* host) -> uint32_t {
  uint32_t mask = 0;
  if (host->message) mask |= TSUGUMI_HOST_MESSAGE;
  if (host->pointer) mask |= TSUGUMI_HOST_POINTER;
  if (host->key) mask |= TSUGUMI_HOST_KEY;
  if (host->audio) mask |= TSUGUMI_HOST_AUDIO;
  if (host->text) mask |= TSUGUMI_HOST_TEXT;
  if (host->composition) mask |= TSUGUMI_HOST_COMPOSITION;
  if (host->gamepad) mask |= TSUGUMI_HOST_GAMEPAD;
  return mask;
}

auto tsugumi_view_host_error(const tsugumi_view_host* host) -> const char* {
  if (!host->dropped.empty()) return host->dropped.c_str();
  return host->gpu.first_error();
}

auto tsugumi_view_host_destroy(tsugumi_view_host* host) -> void { delete host; }
