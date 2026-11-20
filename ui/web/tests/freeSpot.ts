import { cardIds } from './cardIds'
import assert from 'node:assert/strict'
import { get, parse, str } from '../shared/json'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { CanvasController } = await import('../src/model/canvas')
const { ProjectSession } = await import('../src/model/session')
const { WorkflowTab } = await import('../src/model/workspace')

const s = cardIds(new ProjectSession('/p', 'k', 'p'))
s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
s.layout = parse('{}')
s.connected = true
const cv = new CanvasController(s, new WorkflowTab([]))
const here = { x: 4000, y: 4000 }
const checkAutomatic = (id: string) => {
  assert.ok(s.cards([]).some(card => str(get(card, 'id')) === id))
  assert.equal(get(s.layout, 'cards', id), undefined)
}
for (const t of ['draft', 'draft', 'python'] as const) {
  checkAutomatic((await cv.addBuiltin(t, here))!)
}
const undos = s.undoStack.length
checkAutomatic((await cv.addDraft())!)
assert.equal(s.undoStack.length, undos + 1)
const dropped = await s.addCard('draft', [], here)
assert.deepEqual(s.position(dropped), here, 'an aimed drop retains its exact position')
console.log('automatic and explicit Card placement: passed')
