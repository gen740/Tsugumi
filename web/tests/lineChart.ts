import assert from 'node:assert/strict'
import { buildVertices, decodeSeries, relativePosition, visibleRange, palette } from '../../sdk/views/line-geometry.js'
import * as view from '../../sdk/views/line-chart.js'
import { moduleThumbnailKey } from '../src/model/moduleThumbnails'

const pack = (values: number[]) => {
  const bytes = new Uint8Array(values.length * 8), data = new DataView(bytes.buffer)
  values.forEach((v, i) => data.setFloat64(i * 8, v, true))
  return bytes
}
const data = { config: { series: [{ id: 'a', label: 'First', offset: 0, count: 3 }, { id: 'b', label: 'Second', offset: 3, count: 2 }] },
  blobs: { x: pack([1e12, 1e12 + .125, 1e12 + .25, 1e12, 1e12 + .25]), y: pack([1e12, 1e12 + .25, 1e12 + .5, 1e12 + .25, 1e12]) } }
const series = decodeSeries(data)
assert.equal(series[0].x[1] - series[0].x[0], .125)
assert.equal(relativePosition(1e12 + .125, 1e12, .25), .5)
assert.equal(relativePosition(0, -1e308, Infinity, 1e308), .5)
const shifted = series.map(s => ({ ...s, x: s.x.map(x => x - 1e12), y: s.y.map(y => y - 1e12) }))
assert.deepEqual(buildVertices(series, 640, 400), buildVertices(shifted, 640, 400), 'subtract the f64 origin before conversion to f32')
assert.ok(buildVertices(series, 640, 400).every(Number.isFinite))
assert.ok(buildVertices([{ x: [1e300, 1e300], y: [1e300, 1e300] }], 100, 80).every(Number.isFinite))
assert.ok(buildVertices([], 16, 16).every(Number.isFinite))
assert.notDeepEqual(buildVertices(series, 640, 400, 1, true), buildVertices(series, 640, 400, 1, false))
assert.notDeepEqual(buildVertices(series, 640, 400), buildVertices(series, 640, 400, 1, false, { ...visibleRange(series), xmin: 1e12 - .25, xspan: .5 }))
assert.throws(() => decodeSeries({ ...data, blobs: { ...data.blobs, y: pack([NaN]) } }))
assert.throws(() => decodeSeries({ ...data, config: { series: [{ offset: 1, count: 5 }] } }))
assert.notEqual(await moduleThumbnailKey('line-chart', '{}', 100, 80, {}, 'light'), await moduleThumbnailKey('line-chart', '{}', 100, 80, {}, 'dark'))

// Real module lifecycle against a GPU stand-in: uploads, replacement, theme,
// visibility, preview compositing and resource release, without a browser.
let uploads = 0, draws = 0, destroyed = 0, clear: number[] = []
const root: any = { style: {}, children: [], append(...nodes: any[]) { for (const n of nodes) { n.parentElement = this; this.children.push(n) } }, getBoundingClientRect: () => ({ left: 0, top: 0 }) }
const gpu = { getCurrentTexture: () => ({ createView() {} }), configure() {}, unconfigure() { destroyed++ } }
const device: any = {
  createShaderModule: () => ({}), createRenderPipeline: () => ({}),
  createBuffer: () => ({ destroy() { destroyed++ } }), destroy() { destroyed++ },
  queue: { writeBuffer(_: any, __: any, vertices: Float32Array) { assert.ok(vertices.every(Number.isFinite)); uploads++ }, submit() { draws++ }, async onSubmittedWorkDone() {} },
  createCommandEncoder: () => ({ beginRenderPass(options: any) { clear = options.colorAttachments[0].clearValue; return { setPipeline() {}, setVertexBuffer() {}, draw() {}, end() {} } }, finish() {} }),
}
const original = { navigator: Object.getOwnPropertyDescriptor(globalThis, 'navigator'), document: globalThis.document, usage: (globalThis as any).GPUBufferUsage }
Object.defineProperty(globalThis, 'navigator', { configurable: true, value: { gpu: { async requestAdapter() { return { async requestDevice() { return device } } }, getPreferredCanvasFormat: () => 'bgra8unorm' } } })
Object.assign(globalThis, { GPUBufferUsage: { VERTEX: 1, COPY_DST: 2 }, document: { createElement(tag: string) {
  const node: any = { style: {}, children: [], offsetHeight: 20, remove() {}, replaceChildren() { this.children = [] }, append(n: any) { this.children.push(n) }, getBoundingClientRect: () => ({ left: 12, top: 100 }) }
  if (tag === 'canvas') Object.assign(node, { getContext: (kind: string) => kind === 'webgpu' ? gpu : { fillRect() {}, drawImage() {}, fillText() {} }, toDataURL: () => 'data:image/png;base64,test' })
  return node
} } })
try {
  const size = { width: 400, height: 300, pixelRatio: 2, visible: true, colorScheme: 'light' }
  await view.initialise(root, data, size)
  assert.deepEqual(clear, [1, 1, 1, 1]); assert.equal(root.children[1].children.length, 2)
  assert.equal(root.children[1].children[1].style.color, `rgb(${palette[1].map(c => Math.round(c * 255)).join(',')})`)
  await view.update({ ...data, blobs: { ...data.blobs, y: pack([1, 3, 5, 4, 2]) } })
  assert.ok(uploads >= 2)
  view.resize({ ...size, colorScheme: 'dark' }); assert.deepEqual(clear, [.06, .07, .09, 1])
  const before = draws; view.resize({ ...size, visible: false }); assert.equal(draws, before)
  view.resize(size); assert.equal(await view.snapshot(), 'data:image/png;base64,test')
  view.dispose(); assert.equal(destroyed, 3)
} finally {
  if (original.navigator) Object.defineProperty(globalThis, 'navigator', original.navigator)
  Object.assign(globalThis, { document: original.document, GPUBufferUsage: original.usage })
}
console.log('line chart: double precision, binary input, series, updates, themes, previews and disposal passed')
