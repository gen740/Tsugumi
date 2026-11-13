// Only the portable Tsugumi boundary. WebGPU marshaling is entirely Emdawnwebgpu.
addToLibrary({
  tsugumi_view_config__deps: [],
  tsugumi_view_config: size => {
    const input = Module['tsugumiInput'];
    HEAPU32[size >> 2] = input.config.size;
    return input.config.ptr;
  },
  tsugumi_view_blob__deps: ['$UTF8ToString'],
  tsugumi_view_blob: (name, size) => {
    const blob = Module['tsugumiInput'].blobs.get(UTF8ToString(name));
    HEAPU32[size >> 2] = blob?.size ?? 0;
    return blob?.ptr ?? 0;
  },
  tsugumi_view_size: ptr => {
    const size = Module['tsugumiSize'];
    HEAPF32[ptr >> 2] = size.width;
    HEAPF32[(ptr + 4) >> 2] = size.height;
    HEAPF32[(ptr + 8) >> 2] = size.pixelRatio;
  },
  tsugumi_view_on_pointer: callback => { Module['tsugumiCallbacks'].pointer = callback; },
  tsugumi_view_on_wheel: callback => { Module['tsugumiCallbacks'].wheel = callback; },
  tsugumi_view_on_frame: callback => {
    Module['tsugumiCallbacks'].frame = callback;
    Module['tsugumiSchedule']();
  },
  tsugumi_view_error__deps: ['$UTF8ToString'],
  tsugumi_view_error: message => { throw new Error(UTF8ToString(message)); },
  // These private imports adapt host-owned browser GPU objects to Dawn handles.
  // Card code never sees JavaScript, a selector or a DOM object.
  tsugumi_host_device__deps: ['$WebGPU'],
  tsugumi_host_device: () => WebGPU.importJsDevice(Module['tsugumiDevice']),
  tsugumi_host_surface__deps: ['$WebGPU'],
  tsugumi_host_surface: () => WebGPU.importJsSurface(Module['canvas'].getContext('webgpu')),
});
