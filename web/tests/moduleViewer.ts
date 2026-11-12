import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import vm from 'node:vm'
const script = fs.readFileSync(path.join(process.argv[2], 'ui/modules/viewer.js'), 'utf8')
let handler: ((event: any) => void) | undefined
let pagehide: (() => void) | undefined
const messages: any[] = [], draws: string[] = []
let finish: (() => void) | undefined, finalized = 0, purged = 0
const view = { replaceChildren() {} }
const context = {
  console,
  document: { body: { dataset: {}, classList: { toggle() {}, remove() {} } }, getElementById: (id: string) => id === 'view' ? view : { textContent: '' } },
  window: {
    innerWidth: 0, innerHeight: 0,
    parent: { postMessage: (m: any) => messages.push(m) },
    addEventListener: (kind: string, f: any) => { if (kind === 'message') handler = f; if (kind === 'pagehide') pagehide = f },
    Plotly: {
      react: async (_: any, data: any) => { draws.push(data[0]); await new Promise<void>(resolve => { finish = resolve }) },
      purge: () => purged++, toImage: async (_: any, options: any) => { assert.equal(options.width, 400); assert.equal(options.height, 300); return 'data:image/png;base64,plot' },
    },
    vega: { loader: () => ({}) },
    vegaEmbed: async (_: any, spec: any) => { draws.push(spec.description); return { finalize: () => finalized++, view: { toImageURL: async () => 'data:image/png;base64,vega' } } },
  } as any,
}
vm.createContext(context); vm.runInContext(script, context)
const tick = () => new Promise(resolve => setTimeout(resolve, 0))
const lib = (name: string) => handler!({ source: context.window.parent, data: { tsugumi: 'lib', lib: name, code: '' } })
const first = context.window.tsugumiRender({ name: 'plotly', json: '{"data":["old"]}', requestId: 1 })
lib('plotly.min.js'); await tick()
const obsolete = context.window.tsugumiRender({ name: 'plotly', json: '{"data":["obsolete"]}', requestId: 2 })
const latest = context.window.tsugumiRender({ name: 'plotly', json: '{"data":["latest"]}', requestId: 3, snapshot: true, width: 400, height: 300 })
assert.equal(await obsolete, false)
finish!(); await first; await tick(); assert.deepEqual(draws, ['old', 'latest'])
finish!(); await latest
assert.equal(messages.filter(m => m.tsugumi === 'state').length, 1, 'obsolete completion is not reported')
assert.equal(messages.at(-1).requestId, 3); assert.equal(messages.at(-1).image, 'data:image/png;base64,plot')
const vega = context.window.tsugumiRender({ name: 'vega-lite', json: '{"description":"vega-1"}' })
for (const name of ['vega.min.js', 'vega-lite.min.js', 'vega-embed.min.js']) { await tick(); lib(name) }
await vega
assert.equal(purged, 1)
await context.window.tsugumiRender({ name: 'vega-lite', json: '{"description":"vega-2"}' })
assert.equal(finalized, 1, 'replaced Vega view releases listeners and timers')
pagehide!(); assert.equal(finalized, 2, 'closing the viewer releases its last Vega view')
console.log('module viewer: serialized latest updates, snapshot identity and resource cleanup passed')
