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
const { CanvasController, noteRect, Scene } = await import('../src/model/canvas')
const { intersects } = { intersects: (a: Rect, b: Rect) => a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height }
const { ProjectSession } = await import('../src/model/session')
const { WorkflowTab } = await import('../src/model/workspace')
type Rect = { x: number; y: number; width: number; height: number }

const s = cardIds(new ProjectSession('/p', 'k', 'p'))
s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
s.layout = parse('{}')
s.scheduleSave = () => {} // no tsugumid here
s.connected = true
const cv = new CanvasController(s, new WorkflowTab([]))
const covers = (k: string) => {
  const scene = new Scene(s, [])
  const r = scene.cardRect(k)
  return [
    ...scene.cardKeys.filter((c) => c !== k && intersects(r, scene.cardRect(c))),
    ...scene.outputKeys.filter((c) => c !== k && intersects(r, scene.frameRect(c))).map((c) => `frame ${c}`),
    ...s.notes([]).filter((n) => intersects(r, noteRect(n))).map(() => 'note'),
  ]
}

const here = { x: 4000, y: 4000 }
const first = (await cv.addBuiltin('draft', here))!
assert.ok(Math.abs(s.position(first).x - here.x) < 40, 'a free place is taken')
const second = (await cv.addBuiltin('draft', here))!
const third = (await cv.addBuiltin('python', here))!
// Near a frame and a note too.
const framed = await s.addCard('draft', [], { x: 9000, y: 0 })
s.updateDisplayFrame(framed, (f) => {
  for (const [name, v] of [['x', '6000'], ['y', '6000'], ['width', '320'], ['height', '170']]) f.set(name, parse(v))
})
const onFrame = (await cv.addBuiltin('draft', { x: 6100, y: 6050 }))!
s.addNote([], { x: 8000, y: 8000 })
const onNote = (await cv.addBuiltin('draft', { x: 8050, y: 8020 }))!
for (const k of [first, second, third, onFrame, onNote]) assert.deepEqual(covers(k), [], k)

// E too, and one undo takes the Card away with its place.
cv.setCamera({ x: -4000, y: -4000, zoom: 1 }, false)
const undos = s.undoStack.length
const pressedE = (await cv.addDraft())!
assert.deepEqual(covers(pressedE), [])
assert.equal(s.undoStack.length, undos + 1)

// Aimed at a spot without asking for room: stays there (a drop).
const dropped = await s.addCard('draft', [], here)
assert.deepEqual(s.position(dropped), here)
console.log('free spot behavior: passed')
