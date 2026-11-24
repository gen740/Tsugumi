import assert from 'node:assert/strict'
import { absoluteNow, DawnViewInstance } from '../src/dawn/instance'
import { viewKeysAttribute, viewOwnsKeys } from '../src/dawn/input'
import { loadView } from '../src/dawn/loadView'
import { encodeView, f32, i32, i64, uleb } from './dawnFixture'

const frames = new Map<number, FrameRequestCallback>()
let nextFrame = 0
let loseDevice!: (value: { message: string }) => void
const device = {
  queue: {},
  lost: new Promise<{ message: string }>(resolve => { loseDevice = resolve }),
  destroy() {},
  onuncapturederror: null as null | ((event: { error: { message: string } }) => void),
}
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
  width: 300, height: 150,
  getContext: (name: string) => name === 'webgpu' ? context : null,
})
let deviceRequests = 0
const globals = {
  navigator: { gpu: {
    requestAdapter: async () => ({ requestDevice: async () => {
      if (deviceRequests++) device.lost = new Promise(() => {})
      return device
    } }),
    getPreferredCanvasFormat: () => 'bgra8unorm',
  } },
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
  draw(performance.now())
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
const send = (ptr: number, size: number, route = 128) => [...constant(route), ...constant(ptr), ...constant(size), 0x10, 0]
const read = (ptr: number, size: number) => [...constant(ptr), ...constant(size), 0x10, 1]
const expect = (expression: number[], expected: number) =>
  [...expression, ...constant(expected), 0x47, 0x04, 0x40, 0x00, 0x0b]
const load = (address: number) => [...constant(address), 0x2d, 0, 0]
const store = (address: number, value: number[]) => [...constant(address), ...value, 0x3a, 0, 0]
const mark = (value: number) => [...store(0, constant(value)), ...expect(send(0, 1), 0)]

const max = 1 << 24
const { module } = await loadView(encodeView({
  imports: [
    { module: 'tsugumi', name: 'tsugumi_view_send', params: [i32, i32, i32] },
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
      params: [i32, i64],
      code: [
        ...expect([0x20, 0, 0x45], 0), ...expect(read(0, 1), -1), ...mark(3),
        0x20, 0, 0x10, 2,
        // The first draw returns bit 0, subsequent draws return only bit 1.
        ...store(5, [...load(5), ...constant(1), 0x6a]), ...load(5), ...constant(1), 0x46,
        0x04, i32, ...constant(1), 0x05, ...constant(2), 0x0b,
      ],
    },
    resize: { params: [i32, i32, f32, i64], code: [...expect([0x20, 0], 841), ...mark(4), ...constant(0)] },
    text: { params: [i32, i64], code: [
      ...expect([0x20, 0], 3), ...expect(read(16, 3), 0),
      ...expect(load(16), 230), ...expect(load(17), 151), ...expect(load(18), 165),
      ...mark(6), ...constant(0),
    ] },
    composition: { params: [i32, i32, i64], code: [
      ...expect([0x20, 0], 3), ...expect([0x20, 1], 3), ...expect(read(16, 3), 0),
      ...mark(7), ...constant(0),
    ] },
    audio: { params: [i32, i64], code: [
      ...expect([0x20, 0], 1), ...expect([0x20, 1, 0x50], 0),
      ...store(35, constant(63)), ...store(39, constant(191)), ...constant(32),
    ] },
    gamepad: { params: [i32, i32, i32, f32, f32, f32, f32, f32, f32, i64], code: [
      ...expect([0x20, 0], 3), ...expect([0x20, 2], 9), ...mark(8), ...constant(0),
    ] },
    message: {
      params: [i32, i64],
      code: [
        ...expect([0x20, 0], 1),
        ...expect(read(4, 0), -1), ...expect(read(-1, 1), -1), ...expect(read(4, 1), 0),
        ...load(4), ...constant(255), 0x46, 0x04, 0x40, 0x00, 0x0b,
        ...load(4), ...constant(10), 0x46, 0x04, 0x40,
        ...expect(send(-1, 1), -1), ...expect(send(0, 1, -1), -1), ...expect(send(0, max + 1), -1),
        ...store(128, constant(171)), ...expect(send(4, max), 0),
        ...store(128, constant(205)), ...expect(send(4, max), 0), ...expect(send(0, 1), -1),
        0x0b,
        ...constant(0),
      ],
    },
  },
}))

const sent: [number, number][] = []
const fatal: unknown[] = []
const routes: number[][] = []
const gpuErrors: string[] = []
let release = () => {}
let hold = false
const sendErrors: unknown[] = []
const host = {
  send: async (bytes: Uint8Array, route: Uint8Array) => {
    routes.push([...route])
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
  acceptsRoute: () => true,
  onCall() {},
  onTextArea() {},
}
const original = new Map(Object.keys(globals).map(name => [name, Object.getOwnPropertyDescriptor(globalThis, name)]))
try {
  for (const [name, value] of Object.entries(globals))
    Object.defineProperty(globalThis, name, { value, configurable: true })
  const mount = (module: WebAssembly.Module) =>
    DawnViewInstance.mount(module, canvas as unknown as OffscreenCanvas, { width: 800, height: 400, scale: 2 }, host, 1000 / 60, true)
  const mounted = await mount(module)
  await tick()
  assert.equal(deviceRequests, 1)
  assert.equal(configs, 1)
  assert.deepEqual(sent, [[1, 1], [1, 2], [1, 3]])
  assert.deepEqual(routes, Array.from({ length: 3 }, () => Array(16).fill(0)))
  assert.equal(draws, 1)
  stepFrame()
  assert.equal(draws, 2)
  assert.equal(frames.size, 0)
  const message = (value: number) => mounted.deliver(Uint8Array.of(value), absoluteNow())
  await tick()
  hold = true
  message(10)
  message(20)
  assert.equal(frames.size, 1)
  await tick()
  assert.deepEqual(sent.slice(-1), [[max, 10]])
  release()
  await tick()
  mounted.resize({ width: 841, height: 420, scale: 2 }, absoluteNow())
  assert.equal(canvas.width, 841)
  assert.equal(canvas.height, 420)
  stepFrame()
  assert.equal(draws, 3)
  assert.equal(frames.size, 0)
  await tick()
  assert.deepEqual(sent.slice(-4), [[max, 10], [max, 10], [1, 4], [1, 3]])
  assert.deepEqual(routes.slice(-4).map(route => route[0]), [171, 205, 205, 205])
  assert.deepEqual(sendErrors.slice(0, 4), [undefined, undefined, undefined, undefined])
  assert.match(String(sendErrors[4]), /send failed/)
  assert.equal(fatal.length, 0)

  mounted.input({ type: 'text', text: '日' }, absoluteNow())
  mounted.input({ type: 'composition', text: '日', cursor: 3 }, absoluteNow())
  mounted.input({ type: 'gamepad', pad: 3, kind: 1, buttons: 9, lx: 0, ly: 0, rx: 0, ry: 0, lt: 0, rt: 0 }, absoluteNow())
  assert.deepEqual([...mounted.audio(1, absoluteNow() + 50)!], [0.5, -0.5])
  await tick()
  assert.deepEqual(sent.slice(-3), [[1, 6], [1, 7], [1, 8]])
  stepFrame()

  // A minimized view keeps its calls; drawing pauses and resumes on restore.
  const shown = draws
  message(20)
  assert.equal(frames.size, 1)
  mounted.setVisible(false)
  assert.equal(frames.size, 0)
  message(20)
  assert.equal(frames.size, 0)
  assert.equal(draws, shown)
  mounted.setVisible(true)
  assert.equal(draws, shown + 1)

  device.onuncapturederror?.({ error: { message: 'validation' } })
  loseDevice({ message: 'device lost' })
  await tick()
  assert.deepEqual(gpuErrors, ['validation', 'device lost'])
  message(20)
  assert.equal(frames.size, 1)
  assert.throws(() => message(255), WebAssembly.RuntimeError)
  assert.ok(fatal[0] instanceof WebAssembly.RuntimeError)
  assert.equal(frames.size, 0)
  assert.equal(configs, 0)
  assert.throws(() => message(10), /view is closed/)
  assert.equal(frames.size, 0)
  mounted.dispose()

  const { module: optionalAbsent } = await loadView(encodeView())
  const fallback = await mount(optionalAbsent)
  stepFrame()
  assert.equal(frames.size, 1)
  fallback.deliver(Uint8Array.of(255), absoluteNow())
  fallback.dispose()
  assert.equal(frames.size, 0)

  const { module: negativeResize } = await loadView(encodeView({
    callbacks: { resize: { params: [i32, i32, f32, i64], code: constant(-1) } },
  }))
  const negative = await mount(negativeResize)
  assert.throws(() => negative.resize({ width: 842, height: 421, scale: 2 }, absoluteNow()), /returned -1/)
  assert.match(String(fatal.at(-1)), /tsugumi_view_resize returned -1/)
  assert.equal(frames.size, 0)
  negative.dispose()

  // A view that takes keys owns them while its canvas has the focus; one without key does not.
  const { module: keyed } = await loadView(encodeView({
    callbacks: { key: { params: [i32, i32, i32, i64], code: constant(0) } },
  }))
  const typing = await mount(keyed)
  assert.equal(deviceRequests, 4, 'each view owns its own GPU device')
  assert.equal(typing.features.key, true)
  const focused = (owner: boolean) => ({ closest: (selector: string) => owner && selector === `[${viewKeysAttribute}]` ? {} : null })
  assert.equal(viewOwnsKeys(focused(true)), true)
  assert.equal(viewOwnsKeys(focused(false)), false)
  assert.equal(viewOwnsKeys(null), false)
  typing.dispose()
} finally {
  for (const [name, descriptor] of original) {
    if (descriptor) Object.defineProperty(globalThis, name, descriptor)
    else Reflect.deleteProperty(globalThis, name)
  }
}

console.log('Dawn view instance passed')
