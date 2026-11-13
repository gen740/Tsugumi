import createRuntime from './runtime.js'

// tsugumiWasm is injected by tsugumid's ModuleRead, before this shared entry.
let runtime, view, canvas, device, deviceHandle, surfaceHandle, animation = 0, stopped = true
let listeners, failure, format

function check() { if (failure) throw failure }
function fail(error) {
  if (stopped) return
  failure = error instanceof Error ? error : new Error(String(error))
  stopped = true
  cancelAnimationFrame(animation); animation = 0
  // The common isolated-frame host forwards this diagnostic through its bridge.
  canvas?.dispatchEvent(new CustomEvent('tsugumi-view-error', { bubbles: true, detail: failure.message }))
}
function invoke(kind, ...args) {
  const callback = runtime?.tsugumiCallbacks[kind]
  if (!callback || stopped) return
  try { runtime[`_tsugumi_host_${kind}`](callback, ...args) } catch (error) { fail(error) }
}
function schedule() {
  if (stopped || !runtime.tsugumiSize.visible || !runtime.tsugumiCallbacks.frame) {
    cancelAnimationFrame(animation); animation = 0
    return
  }
  if (animation) return
  animation = requestAnimationFrame(time => {
    animation = 0
    invoke('frame', time)
    schedule()
  })
}
function freeInput(input) {
  if (!input) return
  runtime._free(input.config.ptr)
  for (const blob of input.blobs.values()) runtime._free(blob.ptr)
}
function copy(bytes) {
  const ptr = runtime._malloc(Math.max(1, bytes.byteLength))
  if (!ptr) throw new Error('View input allocation failed')
  runtime.HEAPU8.set(bytes, ptr)
  return { ptr, size: bytes.byteLength }
}
function replaceInput({ config, blobs }) {
  const bytes = new TextEncoder().encode(JSON.stringify(config) + '\0')
  const input = { config: copy(bytes), blobs: new Map() }
  --input.config.size
  try {
    for (const [name, blob] of Object.entries(blobs)) input.blobs.set(name, copy(blob))
  } catch (error) { freeInput(input); throw error }
  freeInput(runtime.tsugumiInput)
  runtime.tsugumiInput = input
}

export async function initialise(root, data, size) {
  if (!navigator.gpu) throw new Error('WebGPU is unavailable in this web view')
  failure = undefined; stopped = false
  try {
    const adapter = await navigator.gpu.requestAdapter()
    if (!adapter) throw new Error('WebGPU has no available adapter')
    device = await adapter.requestDevice()
    canvas = document.createElement('canvas')
    canvas.style.cssText = 'width:100%;height:100%;display:block;touch-action:none'
    root.append(canvas)
    listeners = new AbortController()
    runtime = await createRuntime({ canvas, tsugumiDevice: device,
      tsugumiCallbacks: {}, tsugumiSchedule: schedule, tsugumiSize: size,
      // No runtime downloads, including dynamic library dependencies.
      locateFile: () => { throw new Error('External view dependencies are disabled') },
      onAbort: message => fail(new Error(String(message))) })
    replaceInput(data)
    view = await runtime.loadWebAssemblyModule(tsugumiWasm, { loadAsync: true, nodelete: true }, 'card', {})
    for (const name of ['initialise', 'update', 'resize', 'dispose'])
      if (typeof view[`tsugumi_view_${name}`] !== 'function') throw new Error(`Missing view entry: ${name}`)
    deviceHandle = runtime._tsugumi_host_device()
    surfaceHandle = runtime._tsugumi_host_surface()
    format = runtime._tsugumi_host_format(navigator.gpu.getPreferredCanvasFormat() === 'bgra8unorm')
    setSize(size)
    view.tsugumi_view_initialise(deviceHandle, surfaceHandle, format)
    check()
    view.tsugumi_view_update()
    view.tsugumi_view_resize()
    const pointer = (kind, event) => {
      if (event.isPrimary === false) return
      if (kind === 0) canvas.setPointerCapture(event.pointerId)
      const rect = canvas.getBoundingClientRect()
      invoke('pointer', kind, event.clientX - rect.left, event.clientY - rect.top, event.buttons)
    }
    for (const [name, kind] of [['pointerdown', 0], ['pointermove', 1], ['pointerup', 2], ['pointercancel', 3]])
      canvas.addEventListener(name, event => pointer(kind, event), { signal: listeners.signal })
    canvas.addEventListener('wheel', event => {
      event.preventDefault()
      const scale = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? runtime.tsugumiSize.height : 1
      invoke('wheel', event.deltaX * scale, event.deltaY * scale)
    }, { passive: false, signal: listeners.signal })
    device.addEventListener('uncapturederror', event => fail(event.error), { signal: listeners.signal })
    const ownedDevice = device
    device.lost.then(info => { if (device === ownedDevice && info.reason !== 'destroyed') fail(new Error(info.message)) })
    check(); schedule()
  } catch (error) {
    try { await dispose() } catch { /* Preserve the initialization diagnostic. */ }
    throw error
  }
}
function setSize(size) {
  runtime.tsugumiSize = size
  canvas.width = Math.max(1, Math.round(size.width * size.pixelRatio))
  canvas.height = Math.max(1, Math.round(size.height * size.pixelRatio))
  runtime._tsugumi_host_configure(deviceHandle, surfaceHandle, format, canvas.width, canvas.height)
}
export function update(data) {
  check(); replaceInput(data); view.tsugumi_view_update(); check()
}
export function resize(size) {
  check(); setSize(size)
  if (!size.visible) { cancelAnimationFrame(animation); animation = 0 }
  view.tsugumi_view_resize(); check(); schedule()
}
export async function snapshot() {
  check()
  invoke('frame', performance.now())
  check(); await device.queue.onSubmittedWorkDone(); check()
  return canvas.toDataURL('image/png')
}
export async function dispose() {
  stopped = true; cancelAnimationFrame(animation); animation = 0
  listeners?.abort()
  try { view?.tsugumi_view_dispose?.() }
  finally {
    if (surfaceHandle) runtime._wgpuSurfaceRelease(surfaceHandle)
    if (deviceHandle) runtime._wgpuDeviceRelease(deviceHandle)
    freeInput(runtime?.tsugumiInput)
    device?.destroy(); canvas?.remove()
    runtime = view = canvas = device = listeners = undefined
    surfaceHandle = deviceHandle = 0
  }
}
