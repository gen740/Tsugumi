import { cardIds } from './cardIds'
// A new Card put on the canvas covers nothing: not a Card, a frame or a note,
// also when several are put in the same place. A free place is taken as it is.
import assert from 'node:assert/strict'
import { parse } from '../src/shared/json'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { CanvasController } = await import('../src/model/canvas')
const { ProjectSession } = await import('../src/model/session')
const { WorkflowTab } = await import('../src/model/workspace')
const { get } = await import('../src/shared/json')

const s = cardIds(new ProjectSession('/p', 'k', 'p'))
s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
s.layout = parse('{}')
s.scheduleSave = () => {}
s.connected = true
const cv = new CanvasController(s, new WorkflowTab([]))
const here = { x: 4000, y: 4000 }
for (const t of ['draft', 'draft', 'python'] as const) {
  const k = (await cv.addBuiltin(t, here))!
  assert.equal(get(s.layout, 'cards', k), undefined, 'the host supplies automatic coordinates')
}
const undos = s.undoStack.length
const pressedE = (await cv.addDraft())!
assert.equal(get(s.layout, 'cards', pressedE), undefined)
assert.equal(s.undoStack.length, undos + 1)
const dropped = await s.addCard('draft', [], here)
assert.deepEqual(s.position(dropped), here, 'an aimed drop retains its exact position')
console.log('host placement delegation: passed')
