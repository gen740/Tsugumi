// A GPU/DOM stand-in: this test never renders or opens a socket.
// Set TSUGUMI_WASM_TEMPLATE and TSUGUMI_WASM_VIEW to also exercise Emdawn + C.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import vm from 'node:vm'
import { fileURLToPath } from 'node:url'

const root = process.argv[2] ?? path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..')
const real = process.env.TSUGUMI_WASM_TEMPLATE
const source = fs.readFileSync(real ?? path.join(root, 'sdk/views/wasm/index.js'), 'utf8')
const names = ['window', 'document', 'navigator', 'requestAnimationFrame', 'cancelAnimationFrame', '__tsugumiCreateRuntime']
const originals = new Map(names.map(name => [name, Object.getOwnPropertyDescriptor(globalThis, name)]))
const frames = new Map(), calls = [], errors = [], allocations = new Set()
let nextFrame = 0, currentRuntime
const context = { configure(desc) { assert.ok(desc.device) }, getCurrentTexture() { return { createView: () => ({}) } }, unconfigure() {} }
class Canvas extends EventTarget {
  style = {}; width = 0; height = 0
  getContext(kind) { assert.equal(kind, 'webgpu'); return context }
  getBoundingClientRect() { return { left: 10, top: 20 } }
  setPointerCapture(id) { calls.push(['capture', id]) }
  dispatchEvent(event) { if (event.type === 'tsugumi-view-error') errors.push(event.detail); return super.dispatchEvent(event) }
  toDataURL() { return 'data:image/png;base64,test' }
  remove() {}
}
const canvas = new Canvas(); context.canvas = canvas
const device = Object.assign(new EventTarget(), {
  lost: new Promise(() => {}), destroy() { calls.push(['destroy-device']) },
  queue: { writeBuffer() {}, submit() {}, async onSubmittedWorkDone() {} },
  createBuffer(desc) { return { size: desc.size, usage: desc.usage, destroy() {} } },
  createShaderModule(desc) { assert.match(desc.code, /@vertex/); return {} },
  createRenderPipeline(desc) {
    assert.equal(desc.primitive.topology, 'point-list')
    assert.equal(desc.vertex.buffers[0].arrayStride, 12)
    assert.equal(desc.vertex.buffers[0].attributes[0].format, 'float32x3')
    assert.equal(desc.fragment.targets[0].format, 'bgra8unorm')
    return { getBindGroupLayout: () => ({}) }
  },
  createBindGroup() { return {} },
  createCommandEncoder() { return { beginRenderPass() { return {
    setPipeline() {}, setBindGroup() {}, setVertexBuffer() {}, draw(count) { calls.push(['draw', count]) }, end() {},
  } }, finish: () => ({}) } },
})
Object.defineProperties(globalThis, {
  window: { configurable: true, value: {} }, document: { configurable: true, value: { createElement: () => canvas } },
  navigator: { configurable: true, value: { gpu: { async requestAdapter() { return { async requestDevice() { return device } } }, getPreferredCanvasFormat: () => 'bgra8unorm' } } },
  requestAnimationFrame: { configurable: true, value: fn => { const id = ++nextFrame; frames.set(id, fn); return id } },
  cancelAnimationFrame: { configurable: true, value: id => frames.delete(id) },
})
const functions = new Map([[1, (...args) => calls.push(['pointer', ...args])], [2, (...args) => calls.push(['wheel', ...args])],
  [3, time => calls.push(['frame', time])], [4, () => { throw new Error('frame failed') }]])
globalThis.__tsugumiCreateRuntime = async options => {
  let cursor = 1024, library
  const memory = new ArrayBuffer(1 << 20), HEAPU8 = new Uint8Array(memory)
  const runtime = Object.assign(options, { HEAPU8,
    _malloc(size) { const ptr = cursor; cursor += size + 16; allocations.add(ptr); return ptr },
    _free(ptr) { assert.ok(allocations.delete(ptr), 'input allocation freed once') },
    _tsugumi_host_pointer: (callback, ...args) => functions.get(callback)(...args),
    _tsugumi_host_wheel: (callback, ...args) => functions.get(callback)(...args),
    _tsugumi_host_frame: (callback, ...args) => functions.get(callback)(...args),
    _tsugumi_host_device: () => 100, _tsugumi_host_surface: () => 200, _tsugumi_host_format: () => 27,
    _tsugumi_host_configure() {}, _wgpuSurfaceRelease() {}, _wgpuDeviceRelease() {},
    async loadWebAssemblyModule() { return {
      tsugumi_view_initialise(d, s, f) { assert.deepEqual([d, s, f], [100, 200, 27]); runtime.tsugumiCallbacks = { pointer: 1, wheel: 2, frame: 3 } },
      tsugumi_view_update() {}, tsugumi_view_resize() {}, tsugumi_view_dispose() {},
    } },
  })
  vm.runInNewContext(fs.readFileSync(path.join(root, 'sdk/views/wasm/library.js'), 'utf8'), {
    Module: runtime, HEAPU32: new Uint32Array(memory), HEAPF32: new Float32Array(memory),
    UTF8ToString: ptr => new TextDecoder().decode(HEAPU8.subarray(ptr, HEAPU8.indexOf(0, ptr))),
    addToLibrary: value => { library = value },
  })
  currentRuntime = { runtime, library, memory }; return runtime
}
let module
try {
  const wasm = real ? fs.readFileSync(process.env.TSUGUMI_WASM_VIEW) : new Uint8Array()
  const prefix = `const tsugumiWasm=Uint8Array.from(atob('${Buffer.from(wasm).toString('base64')}'),c=>c.charCodeAt(0));\n`
  const body = real ? source : source.replace("import createRuntime from './runtime.js'", 'const createRuntime = globalThis.__tsugumiCreateRuntime')
  module = await import('data:text/javascript;base64,' + Buffer.from(prefix + body).toString('base64'))
  const size = { width: 400, height: 300, pixelRatio: 2, visible: true }
  const data = { config: { count: 2 }, blobs: { points: new Uint8Array(24) } }
  await module.initialise({ append() {} }, data, size)
  assert.equal(canvas.width, 800); assert.equal(canvas.height, 600); assert.equal(frames.size, 1)
  assert.equal(await module.snapshot(), 'data:image/png;base64,test')
  if (!real) {
    const { library, memory, runtime } = currentRuntime
    const heap = new Uint8Array(memory), words = new Uint32Array(memory)
    const config = library.tsugumi_view_config(64)
    assert.equal(new TextDecoder().decode(heap.subarray(config, config + words[16])), '{"count":2}')
    assert.equal(heap[config + words[16]], 0)
    library.tsugumi_view_on_frame(0); assert.equal(frames.size, 0)
    library.tsugumi_view_on_frame(3); assert.equal(frames.size, 1)
    heap.set(new TextEncoder().encode('points\0'), 80)
    assert.ok(library.tsugumi_view_blob(80, 64)); assert.equal(words[16], 24)
    heap.set(new TextEncoder().encode('absent\0'), 80)
    assert.equal(library.tsugumi_view_blob(80, 64), 0); assert.equal(words[16], 0)
    library.tsugumi_view_size(80)
    assert.deepEqual(Array.from(new Float32Array(memory, 80, 3)), [400, 300, 2])
    canvas.dispatchEvent(Object.assign(new Event('pointerdown'), { pointerId: 5, clientX: 30, clientY: 50, buttons: 1 }))
    assert.deepEqual(calls.find(c => c[0] === 'pointer'), ['pointer', 0, 20, 30, 1])
    await module.resize({ ...size, height: 100, visible: false }); assert.equal(frames.size, 0)
    canvas.dispatchEvent(Object.assign(new Event('wheel'), { deltaMode: 2, deltaX: 1, deltaY: 2 }))
    assert.deepEqual(calls.find(c => c[0] === 'wheel'), ['wheel', 100, 200])
    const old = runtime.tsugumiInput.config.ptr
    await module.update({ config: { count: 0 }, blobs: {} })
    assert.ok(!allocations.has(old)); assert.equal(allocations.size, 1)
    library.tsugumi_view_on_frame(0); await module.resize(size); assert.equal(frames.size, 0)
    library.tsugumi_view_on_frame(4)
    const [id, frame] = frames.entries().next().value; frames.delete(id); frame(20)
    assert.deepEqual(errors, ['frame failed']); assert.equal(frames.size, 0)
    assert.throws(() => module.update(data), /frame failed/)
  } else {
    assert.ok(calls.some(call => call[0] === 'draw' && call[1] === 2))
    canvas.dispatchEvent(Object.assign(new Event('pointerdown'), { pointerId: 5, clientX: 30, clientY: 50, buttons: 1 }))
    canvas.dispatchEvent(Object.assign(new Event('pointermove'), { pointerId: 5, clientX: 40, clientY: 60, buttons: 1 }))
    canvas.dispatchEvent(Object.assign(new Event('wheel'), { deltaMode: 0, deltaX: 0, deltaY: 10 }))
    await module.update(data); await module.resize({ ...size, visible: false }); assert.equal(frames.size, 0)
    assert.throws(() => module.update({ config: { count: 3 }, blobs: data.blobs }), /Expected float32 XYZ triples/)
  }
  await module.dispose(); await module.dispose()
  assert.equal(frames.size, 0); assert.equal(allocations.size, 0)
  assert.ok(calls.some(call => call[0] === 'destroy-device'))
  console.log(`Wasm view: ${real ? 'Emdawnwebgpu + C fixture on a GPU stand-in' : 'input ABI, events, visibility, errors, snapshot and disposal'} passed`)
} finally {
  await module?.dispose()
  for (const [name, descriptor] of originals) {
    if (descriptor) Object.defineProperty(globalThis, name, descriptor)
    else delete globalThis[name]
  }
}
