import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import vm from 'node:vm'
import { moduleSource, registerModuleClient } from '../src/model/moduleSources'
import { moduleThumbnailKey } from '../src/model/moduleThumbnails'

// Exercise the real lifecycle runtime with a DOM stand-in and a real ES module.
const script = fs.readFileSync(path.join(process.argv[2], 'ui/modules/viewer.js'), 'utf8')
const runtime = script.slice(script.indexOf('function runtime()'), script.indexOf('frame.srcdoc ='))
const messages: any[] = [], calls: any[] = []
const listeners = new Map<string, (event: any) => any>()
const root = { style: {}, replaceChildren() {}, calls, textContent: '' }
const parent = { postMessage: (message: any) => messages.push(message) }
const context = vm.createContext({
  document: { createElement: () => root, body: { append() {} } }, parent,
  innerWidth: 400, innerHeight: 300, devicePixelRatio: 2,
  addEventListener: (name: string, fn: any) => listeners.set(name, fn),
  ResizeObserver: class { observe() {} }, setTimeout, requestAnimationFrame: (fn: any) => setTimeout(fn, 0),
  Blob: class { constructor(public parts: string[]) {} },
  URL: { createObjectURL: (blob: { parts: string[] }) => 'data:text/javascript;base64,' + Buffer.from(blob.parts.join('')).toString('base64'), revokeObjectURL() {} },
})
const start = vm.compileFunction(runtime + '; runtime()', [], {
  parsingContext: context, importModuleDynamically: vm.constants.USE_MAIN_CONTEXT_DEFAULT_LOADER,
})
start()
assert.equal(messages[0].tsugumi, 'inner-ready')
const code = `let root;
export function initialise(element, data, size) { root = element; root.calls.push(['init', data.blobs.points[1], size.width]); }
export function update(data) { root.calls.push(['update', data.config.count]); }
export function resize(size) { root.calls.push(['resize', size.visible]); }
export function dispose() { root.calls.push(['dispose']); }
export function snapshot() { return 'data:image/png;base64,test'; }`
const send = (requestId: number, extra = {}) => listeners.get('message')!({ source: parent, data: {
  tsugumi: 'render', name: 'immutable-view', code, json: '{"count":2}', blobs: { points: new Uint8Array([0, 255]) }, requestId, ...extra,
} })
await send(1)
assert.deepEqual(calls[0], ['init', 255, 400])
await send(2, { snapshot: true })
assert.deepEqual(calls[1], ['update', 2])
assert.equal(messages.at(-1).image, 'data:image/png;base64,test')
await listeners.get('message')!({ source: parent, data: { tsugumi: 'visibility', visible: false } })
assert.deepEqual(calls.at(-1), ['resize', false])
await send(3, { name: 'another-view', code: code + '\n// another identity' })
assert.ok(calls.some(call => call[0] === 'dispose'))
listeners.get('pagehide')!({})
assert.equal(calls.at(-1)[0], 'dispose')

// The actual RPC loading path pages bytes, coalesces consumers and retries failures.
let reads = 0
const client = { async call(_: unknown, request: { offset: bigint }) {
  ++reads
  const first = request.offset === 0n
  return { bytes: first ? [65, 0] : [66], offset: request.offset, nextOffset: first ? 2n : 3n, eof: !first }
} }
registerModuleClient(client as never)
const [a, b] = await Promise.all([moduleSource('test-identity'), moduleSource('test-identity')])
assert.equal(a, 'A\0B'); assert.equal(a, b); assert.equal(reads, 2)
const keyA = await moduleThumbnailKey('view', '{}', 100, 100, { points: new Uint8Array([0, 255]) })
const keyB = await moduleThumbnailKey('view', '{}', 100, 100, { points: new Uint8Array([0, 254]) })
assert.notEqual(keyA, keyB)
assert.match(script, /frame.sandbox = 'allow-scripts'/)
assert.match(script, /connect-src 'none'/)
console.log('module views: ES lifecycle, blobs, visibility, snapshots, RPC paging and cache passed')
