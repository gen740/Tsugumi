// A marquee catches Cards, sticky notes and output frames. They move together in
// one edit, and go together: a frame alone is closed, its Card stays.
import assert from 'node:assert/strict'
import { bool, dbl, get, parse } from '../src/shared/json'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { CanvasController, noteRect, Scene } = await import('../src/model/canvas')
const { snapToGrid } = await import('../src/model/canvasKit')
const { ProjectSession } = await import('../src/model/session')
const { WorkflowTab } = await import('../src/model/workspace')

const setup = () => {
  const s = new ProjectSession('/p', 'k', 'p')
  s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  s.layout = parse('{}')
  s.scheduleSave = () => {} // no tsugumid here
  const near = s.addCard('draft', [], { x: 0, y: 0 }), far = s.addCard('draft', [], { x: 5000, y: 0 })
  const note = s.addNote([], { x: 100, y: 300 }), farNote = s.addNote([], { x: 5000, y: 3000 })
  // A frame with a place of its own, and one drawn beside its Card.
  s.updateDisplayFrame(far, (f) => f.set('hidden', false))
  const tab = new WorkflowTab([])
  const cv = new CanvasController(s, tab)
  return { s, near, far, note, farNote, tab, cv, undos: s.undoStack.length }
}
const everything = { x: -100, y: -100, width: 1000, height: 1000 }
const notePlace = (s: InstanceType<typeof ProjectSession>, id: string) => noteRect(s.note(id))

{
  // What the marquee touches, and nothing else.
  const { s, near, far, note, tab } = setup()
  const placed = s.addCard('draft', [], { x: 2000, y: 0 })
  s.updateDisplayFrame(placed, (f) => {
    f.set('x', parse('400'))
    f.set('y', parse('400'))
  })
  const items = new Scene(s, []).itemsIn(everything)
  assert.deepEqual(items.cards, [near])
  assert.deepEqual(items.notes, [note])
  assert.deepEqual(items.frames, [placed], 'the frame caught, though its Card is outside')
  assert.ok(!items.frames.includes(far), 'a frame outside is not')

  // Choosing Cards anew lets go of the rest; so does entering another scope.
  tab.setSelection(items.cards, items.notes, items.frames)
  assert.equal(tab.selectedNotes.size + tab.selectedFrames.size, 2)
  tab.setSelection([near])
  assert.equal(tab.selectedNotes.size + tab.selectedFrames.size, 0)
  tab.setSelection(items.cards, items.notes, items.frames)
  tab.enter(['child'])
  assert.equal(tab.selection.size + tab.selectedNotes.size + tab.selectedFrames.size, 0)
  tab.goBack()
}

{
  // All together, as one edit.
  const { s, near, far, note, cv } = setup()
  const placed = s.addCard('draft', [], { x: 2000, y: 0 })
  s.updateDisplayFrame(placed, (f) => {
    f.set('x', parse('400'))
    f.set('y', parse('400'))
    f.set('width', parse('300'))
    f.set('height', parse('200'))
  })
  const before = { card: s.position(near), note: notePlace(s, note), undos: s.undoStack.length }
  const frame = new Scene(s, []).frameRect(placed)

  // On the move the frame follows, without touching the document.
  assert.equal(new Scene(s, [], undefined, { cards: { keys: new Set([near]), frames: new Set([placed]), dx: 7, dy: 9 } }).frameRect(placed).x, frame.x + 7)
  assert.equal(new Scene(s, []).frameRect(placed).x, frame.x)

  cv.moveSelection(new Set([near]), new Set([note]), new Set([placed]), 30, 40)
  assert.deepEqual(s.position(near), { x: before.card.x + 30, y: before.card.y + 40 })
  assert.deepEqual([notePlace(s, note).x, notePlace(s, note).y], [before.note.x + 30, before.note.y + 40])
  assert.deepEqual([dbl(get(s.displayFrame(placed), 'x')), dbl(get(s.displayFrame(placed), 'y'))], [430, 440])
  assert.equal(dbl(get(s.displayFrame(placed), 'width')), 300, 'its size stays')
  assert.equal(s.undoStack.length, before.undos + 1, 'one edit')
  assert.deepEqual(s.position(far), { x: 5000, y: 0 }, 'what is not selected stays')

  s.undo()
  assert.deepEqual(s.position(near), before.card)
  assert.deepEqual([notePlace(s, note).x, notePlace(s, note).y], [before.note.x, before.note.y])
  assert.equal(dbl(get(s.displayFrame(placed), 'x')), 400, 'one undo brings all back')
}

{
  // A frame beside its Card goes with the Card: moved twice it would leave it.
  const { s, near, cv } = setup()
  s.updateDisplayFrame(near, (f) => f.set('hidden', false))
  const offset = (scene: InstanceType<typeof Scene>) => scene.frameRect(near).x - scene.origin(near).x
  const was = offset(new Scene(s, []))
  assert.equal(offset(new Scene(s, [], undefined, { cards: { keys: new Set([near]), frames: new Set([near]), dx: 50, dy: 0 } })), was)
  cv.moveSelection(new Set([near]), new Set(), new Set([near]), 50, 0)
  assert.equal(dbl(get(s.displayFrame(near), 'x')), undefined, 'still no place of its own')
  assert.equal(offset(new Scene(s, [])), was)

  // Alone, it gets a place of its own where it was dropped.
  const r = new Scene(s, []).frameRect(near)
  cv.moveSelection(new Set(), new Set(), new Set([near]), 11, 13)
  assert.deepEqual([dbl(get(s.displayFrame(near), 'x')), dbl(get(s.displayFrame(near), 'y'))], [Math.round(r.x + 11), Math.round(r.y + 13)])
  assert.deepEqual(s.position(near), { x: 50, y: 0 }, 'its Card stays')
}

{
  // Deleting: a frame alone is closed and its Card stays; notes go; one undo.
  const { s, near, note, tab, cv } = setup()
  s.updateDisplayFrame(near, (f) => f.set('hidden', false))
  s.connected = true
  const undos = s.undoStack.length
  tab.setSelection([], [note], [near])
  cv.deleteSelection()
  assert.equal(bool(get(s.displayFrame(near), 'hidden')), true)
  assert.equal(typeof dbl(get(s.displayFrame(near), 'width')), 'number', 'with its place and size, which the core wants')
  assert.equal(typeof dbl(get(s.displayFrame(near), 'x')), 'number')
  assert.ok(s.instance(near), 'the Card stays')
  assert.equal(s.note(note), undefined)
  assert.equal(s.undoStack.length, undos + 1)
  assert.equal(tab.selectedNotes.size + tab.selectedFrames.size, 0)
  s.undo()
  assert.ok(s.note(note), 'one undo brings all back')
  assert.notEqual(bool(get(s.displayFrame(near), 'hidden')), true)

  // A Card and its frame: the Card takes the frame with it.
  tab.setSelection([near], [], [near])
  cv.deleteSelection()
  assert.equal(s.instance(near), undefined)
  assert.equal(s.displayFrame(near), undefined)
}
{
  // With the grid, all go by the same whole grids, so the arrangement stays:
  // the Card goes from where it is drawn, what moves with it by the same step.
  const { s, note, cv } = setup()
  const offGrid = s.addCard('draft', [], { x: 3010, y: 950 })
  const framed = s.addCard('draft', [], { x: 2000, y: 0 })
  s.updateDisplayFrame(framed, (f) => {
    for (const [name, v] of [['x', '400'], ['y', '400'], ['width', '300'], ['height', '200']]) f.set(name, parse(v))
  })
  const was = notePlace(s, note)
  cv.moveSelection(new Set([offGrid]), new Set([note]), new Set([framed]), 33, 47, 20)
  assert.deepEqual(s.position(offGrid), { x: snapToGrid(3010, 20) + 40, y: snapToGrid(950, 20) + 40 })
  assert.deepEqual([notePlace(s, note).x, notePlace(s, note).y], [was.x + 40, was.y + 40])
  assert.deepEqual([dbl(get(s.displayFrame(framed), 'x')), dbl(get(s.displayFrame(framed), 'y'))], [440, 440])
  s.undo()
  // Without a Card there is nothing to keep to the grid: they land where dropped.
  cv.moveSelection(new Set(), new Set([note]), new Set([framed]), 33, 47, 20)
  assert.deepEqual([notePlace(s, note).x, notePlace(s, note).y], [was.x + 33, was.y + 47])
  assert.deepEqual([dbl(get(s.displayFrame(framed), 'x')), dbl(get(s.displayFrame(framed), 'y'))], [433, 447])
}
console.log('marquee behavior: passed')
