import assert from 'node:assert/strict'
import { DawnViewInstance } from '../src/dawn/instance'
import { loadView } from '../src/dawn/loadView'
import { encodeView, f32, i32, uleb } from './dawnLoad'

const frames = new Map<number, FrameRequestCallback>()
let nextFrame = 0
const window = Object.assign(new EventTarget(), { devicePixelRatio: 2 })
let resize!: ResizeObserverCallback
class Observer {
  constructor(callback: ResizeObserverCallback) { resize = callback }
  observe() {}
  disconnect() {}
}
let loseDevice!: (value: { message: string }) => void
const device = {
  queue: {},
  lost: new Promise<{ message: string }>(resolve => { loseDevice = resolve }),
  onuncapturederror: null as null | ((event: { error: { message: string } }) => void),
  destroy() { ++destroyed },
}
let destroyed = 0
let configs = 0
let draws = 0
const context = {
  configure(config: unknown) {
    assert.deepEqual(config, { device, format: 'bgra8unorm', alphaMode: 'opaque' })
    ++configs
  },
  unconfigure() { --configs },
  getCurrentTexture() { return { createView() { ++draws; return {} } } },
}
const canvas = Object.assign(new EventTarget(), {
  width: 300, height: 150, clientWidth: 400, clientHeight: 200,
  getContext: (name: string) => name === 'webgpu' ? context : null,
})
let deviceRequests = 0
const globals = {
  navigator: { gpu: {
    requestAdapter: async () => ({ requestDevice: async () => { ++deviceRequests; return device } }),
    getPreferredCanvasFormat: () => 'bgra8unorm',
  } },
  window,
  ResizeObserver: Observer,
  requestAnimationFrame: (draw: FrameRequestCallback) => {
    frames.set(++nextFrame, draw)
    return nextFrame
  },
  cancelAnimationFrame: (id: number) => frames.delete(id),
}

const stepFrame = () => {
  assert.equal(frames.size, 1)
  const [[id, draw]] = frames
  frames.delete(id)
  draw(0)
}
const tick = () => new Promise<void>(resolve => setImmediate(resolve))

// The fixture marks callback order by sending a byte. Failed assertions trap.
const constant = (n: number) => {
  const bytes = uleb(n >>> 0)
  if (n === -1) return [0x41, 0x7f]
  if (bytes.at(-1)! & 0x40) {
    bytes[bytes.length - 1] |= 0x80
    bytes.push(0)
  }
  return [0x41, ...bytes]
}
const send = (ptr: number, size: number) => [...constant(ptr), ...constant(size), 0x10, 0]
const read = (ptr: number, size: number) => [...constant(ptr), ...constant(size), 0x10, 1]
const expect = (expression: number[], expected: number) =>
  [...expression, ...constant(expected), 0x47, 0x04, 0x40, 0x00, 0x0b]
const load = (address: number) => [...constant(address), 0x2d, 0, 0]
const store = (address: number, value: number[]) => [...constant(address), ...value, 0x3a, 0, 0]
const mark = (value: number) => [...store(0, constant(value)), ...expect(send(0, 1), 0)]

const max = 1 << 24
const { module } = await loadView(encodeView({
  imports: [
    { module: 'tsugumi', name: 'tsugumi_view_send', params: [i32, i32] },
    { module: 'tsugumi', name: 'tsugumi_view_read', params: [i32, i32] },
    { module: 'webgpu', name: 'wgpuTextureViewRelease', params: [i32], results: [] },
  ],
  pages: max / 65536 + 1,
  callbacks: {
    abi: { code: [...mark(1), ...constant(0)] },
    start: {
      params: [i32, i32, i32, i32, i32, f32],
      code: [
        ...expect([0x20, 0, 0x45], 0), ...expect([0x20, 1, 0x45], 0),
        ...expect([0x20, 3], 800), ...expect([0x20, 4], 400),
        ...mark(2), ...constant(0),
      ],
    },
    draw: {
      params: [i32],
      code: [
        ...expect([0x20, 0, 0x45], 0), ...expect(read(0, 1), -1), ...mark(3),
        0x20, 0, 0x10, 2,
        // The first draw returns bit 0, subsequent draws return only bit 1.
        ...store(5, [...load(5), ...constant(1), 0x6a]), ...load(5), ...constant(1), 0x46,
        0x04, i32, ...constant(1), 0x05, ...constant(2), 0x0b,
      ],
    },
    resize: { params: [i32, i32, f32], code: [...expect([0x20, 0], 841), ...mark(4), ...constant(0)] },
    message: {
      params: [i32],
      code: [
        ...expect([0x20, 0], 1),
        ...expect(read(4, 0), -1), ...expect(read(-1, 1), -1), ...expect(read(4, 1), 0),
        ...load(4), ...constant(255), 0x46, 0x04, 0x40, 0x00, 0x0b,
        ...load(4), ...constant(10), 0x46, 0x04, 0x40,
        ...expect(send(-1, 1), -1), ...expect(send(0, max + 1), -1),
        ...expect(send(4, max), 0), ...expect(send(4, max), 0), ...expect(send(0, 1), -1),
        0x0b,
        ...constant(0),
      ],
    },
  },
}))

const sent: [number, number][] = []
const fatal: unknown[] = []
const gpuErrors: string[] = []
let release = () => {}
let hold = false
const sendErrors: unknown[] = []
const host = {
  send: async (bytes: Uint8Array) => {
    sent.push([bytes.length, bytes[0]])
    if (hold) {
      hold = false
      await new Promise<void>(resolve => { release = resolve })
      throw new Error('send failed')
    }
  },
  onSendError: (error: unknown) => { sendErrors.push(error) },
  onGpuError: (message: string) => { gpuErrors.push(message) },
  onFatal: (error: unknown) => { fatal.push(error) },
}
const original = new Map(Object.keys(globals).map(name => [name, Object.getOwnPropertyDescriptor(globalThis, name)]))
try {
  for (const [name, value] of Object.entries(globals))
    Object.defineProperty(globalThis, name, { value, configurable: true })
  const mounted = await DawnViewInstance.mount(module, canvas as unknown as HTMLCanvasElement, host, new AbortController().signal)
  await tick()
  assert.equal(deviceRequests, 1)
  assert.equal(configs, 1)
  assert.deepEqual(sent, [[1, 1], [1, 2], [1, 3]])
  assert.equal(draws, 1)
  stepFrame()
  assert.equal(draws, 2)
  assert.equal(frames.size, 0)
  const wheel = new Event('wheel', { cancelable: true })
  canvas.dispatchEvent(wheel)
  assert.equal(wheel.defaultPrevented, false)
  assert.equal(frames.size, 0)

  let completed = 0
  const message = (value: number) => mounted.deliver({ bytes: new Uint8Array([value]), complete: () => { ++completed } })
  await tick()
  hold = true
  message(10)
  message(20)
  assert.equal(completed, 2)
  assert.equal(frames.size, 1)
  await tick()
  assert.deepEqual(sent.slice(-1), [[max, 10]])
  release()
  await tick()
  resize([{ devicePixelContentBoxSize: [{ inlineSize: 841, blockSize: 420 }] }] as ResizeObserverEntry[], {} as ResizeObserver)
  assert.equal(canvas.width, 841)
  assert.equal(canvas.height, 420)
  stepFrame()
  assert.equal(draws, 3)
  assert.equal(frames.size, 0)
  await tick()
  assert.deepEqual(sent.slice(-4), [[max, 10], [max, 10], [1, 4], [1, 3]])
  assert.deepEqual(sendErrors.slice(0, 3), [undefined, undefined, undefined])
  assert.match(String(sendErrors[3]), /send failed/)
  assert.equal(fatal.length, 0)

  device.onuncapturederror?.({ error: { message: 'validation' } })
  loseDevice({ message: 'device lost' })
  await tick()
  assert.deepEqual(gpuErrors, ['validation', 'device lost'])
  message(20)
  assert.equal(frames.size, 1)
  message(255)
  assert.equal(completed, 4)
  assert.ok(fatal[0] instanceof WebAssembly.RuntimeError)
  assert.equal(frames.size, 0)
  assert.equal(configs, 0)
  assert.equal(destroyed, 1)
  message(10)
  assert.equal(completed, 5)
  assert.equal(frames.size, 0)
  mounted.dispose()
  assert.equal(destroyed, 1)

  const { module: optionalAbsent } = await loadView(encodeView())
  const fallback = await DawnViewInstance.mount(optionalAbsent, canvas as unknown as HTMLCanvasElement, host, new AbortController().signal)
  stepFrame()
  assert.equal(frames.size, 1)
  fallback.deliver({ bytes: new Uint8Array([255]), complete: () => { ++completed } })
  assert.equal(completed, 6)
  fallback.dispose()
  assert.equal(frames.size, 0)

  const { module: negativeResize } = await loadView(encodeView({
    callbacks: { resize: { params: [i32, i32, f32], code: constant(-1) } },
  }))
  const negative = await DawnViewInstance.mount(negativeResize, canvas as unknown as HTMLCanvasElement, host, new AbortController().signal)
  resize([{ devicePixelContentBoxSize: [{ inlineSize: 842, blockSize: 421 }] }] as ResizeObserverEntry[], {} as ResizeObserver)
  assert.match(String(fatal.at(-1)), /tsugumi_view_resize returned -1/)
  assert.equal(frames.size, 0)
  negative.dispose()
} finally {
  for (const [name, descriptor] of original) {
    if (descriptor) Object.defineProperty(globalThis, name, descriptor)
    else Reflect.deleteProperty(globalThis, name)
  }
}

console.log('Dawn view instance passed')
