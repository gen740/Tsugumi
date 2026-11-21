import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import vm from 'node:vm'
import { moduleSource, registerModuleClient } from '../src/model/moduleSources'

const shared = path.join(process.argv[2], 'ui/shared/modules')
const script = fs.readFileSync(path.join(shared, 'viewer.js'), 'utf8')
assert.match(fs.readFileSync(path.join(shared, 'viewer.html'), 'utf8'), /Content-Security-Policy" content="frame-src 'none'"/)
assert.match(script, /frame.sandbox = 'allow-scripts'/)
assert.match(script, /connect-src 'none'/)

const runtime = script.slice(script.indexOf('function runtime()'), script.indexOf('let current'))
const messages: any[] = [], calls: any[] = []
const listeners = new Map<string, (event: any) => void>()
const root = new EventTarget()
const element = Object.assign(root, { style: {}, calls })
const parent = { postMessage: (message: any) => messages.push(message) }
const context = vm.createContext({
  document: { createElement: () => element, body: { append() {} }, documentElement: { style: {} } }, parent, CustomEvent,
  addEventListener: (name: string, fn: (event: unknown) => void) => listeners.set(name, fn),
  Blob: class { constructor(public parts: string[]) {} },
  URL: { createObjectURL: (blob: { parts: string[] }) => 'data:text/javascript;base64,' + Buffer.from(blob.parts.join('')).toString('base64'), revokeObjectURL() {} },
})
const start = vm.compileFunction(runtime + '; runtime()', [], {
  parsingContext: context, importModuleDynamically: vm.constants.USE_MAIN_CONTEXT_DEFAULT_LOADER,
})
start()
assert.equal(messages[0].tsugumi, 'inner-ready')
const code = `export default function mount(root, data) {
  root.calls.push(['mount', data.config, data.blobs.points[1]]);
  root.addEventListener('tsugumi:data', event => root.calls.push(['update', event.detail.config, event.detail.blobs.points[1]]));
}`
const send = (config: string, byte: number, source = parent) => listeners.get('message')!({ source, data: {
  tsugumi: 'render', code, data: { config, blobs: { points: new Uint8Array([0, byte]) } },
} })
send(' { "count":2 } ', 255)
send('{"count":3}', 254)
send('{"count":10}', 1, { postMessage() {} })
send('{"count":4}', 253)
for (let i = 0; i < 10 && calls.length < 3; i++) await new Promise(resolve => setImmediate(resolve))
assert.deepEqual(calls, [['mount', ' { "count":2 } ', 255], ['update', '{"count":3}', 254], ['update', '{"count":4}', 253]])
assert.equal(messages.at(-1).state, 'ok')

// ModuleRead sends self-contained source in pages and coalesces consumers.
let reads = 0
const client = { async call(_: unknown, request: { offset: bigint }) {
  ++reads
  const first = request.offset === 0n
  return { bytes: first ? [65, 0] : [66], offset: request.offset, nextOffset: first ? 2n : 3n, eof: !first }
} }
registerModuleClient(client as never)
const [a, b] = await Promise.all([moduleSource('test-identity'), moduleSource('test-identity')])
assert.equal(a, 'A\0B'); assert.equal(a, b); assert.equal(reads, 2)
console.log('module views: default mount, opaque config, binary updates, and RPC paging passed')
