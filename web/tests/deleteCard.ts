// A deleted Card takes its folder with it: the save names the definition and
// no file in it, because the core refuses a change to a file in a definition
// it is removing. Files elsewhere still go.
import assert from 'node:assert/strict'
import { parse } from '../src/shared/json'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')

const s = new ProjectSession('/p', 'k', 'p')
s.project = parse('{"name":"t","canvas":{"cards":[{"id":"code"}],"connections":[]}}')
s.confirmedProject = s.project
s.layout = parse('{}')
s.confirmedLayout = s.layout
s.scheduleSave = () => {} // no tsugumid here
s.confirmedDefinitions.set('code', { card: parse('{"template":"card"}') })
for (const p of ['definitions/code/_outputs.fbs', 'definitions/code/src/main.py', 'definitions/other/keep.txt']) {
  s.sources.set(p, { path: p, hash: 'h', size: 1n })
}
const sent: unknown[][] = []
s.client = { save: async (_rev: unknown, _op: unknown, _origin: unknown, changes: unknown[]) => { sent.push(changes); return { rev: 1n } } }

s.deleteCards(new Set(['code']), [])
s.removeSource('definitions/other/keep.txt')
s.putSource('definitions/code/unsaved.txt', 'edited after the removal')
assert.ok(s.dirty, 'the removal waits to be saved')
await (s as unknown as { saveOnce(): Promise<void> }).saveOnce()

assert.equal(s.saveError, undefined, s.saveError)
const text = JSON.stringify(sent[0], (_k, v) => (typeof v === 'bigint' ? String(v) : v))
assert.ok(text.includes('"id":"code"'), `the definition goes: ${text}`)
assert.ok(!text.includes('definitions/code/'), `no file of it: ${text}`)
assert.ok(text.includes('definitions/other/keep.txt'), `a file elsewhere still goes: ${text}`)
assert.ok(!s.sourceRemovals.has('definitions/code/_outputs.fbs'), 'nothing left to send')
console.log('delete card behavior: passed')
