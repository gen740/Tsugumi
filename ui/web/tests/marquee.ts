import { projectFixture } from './documentFixture'
import { cardIds } from './cardIds'
// A marquee selects Cards and sticky notes to move or delete in one edit.
import assert from 'node:assert/strict'
import { parse } from '../shared/json'

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
const { prefs } = await import('../src/model/prefs')
const originalGrid = prefs.showGrid
prefs.set('showGrid', false)

const setup = async () => {
  const s = cardIds(new ProjectSession('/p', 'k', 'p'))
  s.project = parse(projectFixture({"name":"t","canvas":{"cards":[],"connections":[]}}))
  s.layout = parse('{}')
  const near = await s.addCard('draft', [], { x: 0, y: 0 }), far = await s.addCard('draft', [], { x: 5000, y: 0 })
  const note = await s.addNote([], { x: 100, y: 300 }), farNote = await s.addNote([], { x: 5000, y: 3000 })
  await s.save()
  const tab = new WorkflowTab([])
  const cv = new CanvasController(s, tab)
  return { s, near, far, note, farNote, tab, cv, undos: s.undoStack.length }
}
const everything = { x: -100, y: -100, width: 1000, height: 1000 }
const notePlace = (s: InstanceType<typeof ProjectSession>, id: string) => noteRect(s.note(id))

{
  // What the marquee touches, and nothing else.
  const { s, near, far, note, tab } = await setup()
  const items = new Scene(s, []).itemsIn(everything)
  assert.deepEqual(items.cards, [near])
  assert.deepEqual(items.notes, [note])

  // Choosing Cards anew lets go of the rest; so does entering another scope.
  tab.setSelection(items.cards, items.notes)
  assert.equal(tab.selectedNotes.size, 1)
  tab.setSelection([near])
  assert.equal(tab.selectedNotes.size, 0)
  tab.setSelection(items.cards, items.notes)
  tab.enter(['child'])
  assert.equal(tab.selection.size + tab.selectedNotes.size, 0)
  tab.goBack()
}

{
  // All together, as one edit.
  const { s, near, far, note, cv } = await setup()
  const before = { card: s.position(near), note: notePlace(s, note), undos: s.undoStack.length }
  await cv.moveSelection(new Set([near]), new Set([note]), 30, 40)
  assert.deepEqual(s.position(near), { x: before.card.x + 30, y: before.card.y + 40 })
  assert.deepEqual([notePlace(s, note).x, notePlace(s, note).y], [before.note.x + 30, before.note.y + 40])
  assert.equal(s.undoStack.length, before.undos + 1, 'one edit')
  assert.deepEqual(s.position(far), { x: 5000, y: 0 }, 'what is not selected stays')

  await s.undo()
  assert.deepEqual(s.position(near), before.card)
  assert.deepEqual([notePlace(s, note).x, notePlace(s, note).y], [before.note.x, before.note.y])
}

{
  // A selected note is deleted and can be restored with undo.
  const { s, near, note, tab, cv } = await setup()
  s.connected = true
  const undos = s.undoStack.length
  tab.setSelection([], [note])
  cv.deleteSelection()
  await s.save()
  assert.ok(s.instance(near), 'the Card stays')
  assert.equal(s.note(note), undefined)
  assert.equal(s.undoStack.length, undos + 1)
  assert.equal(tab.selectedNotes.size, 0)
  await s.undo()
  assert.ok(s.note(note), 'one undo brings all back')

  tab.setSelection([near])
  cv.deleteSelection()
  await s.save()
  assert.equal(s.instance(near), undefined)
}
{
  // With the grid, all go by the same whole grids, so the arrangement stays:
  // the Card goes from where it is drawn, what moves with it by the same step.
  prefs.set('showGrid', true)
  const { s, note, cv } = await setup()
  const offGrid = await s.addCard('draft', [], { x: 3010, y: 950 })
  const was = notePlace(s, note)
  await cv.moveSelection(new Set([offGrid]), new Set([note]), 33, 47, 20)
  assert.deepEqual(s.position(offGrid), { x: snapToGrid(3010, 20) + 40, y: snapToGrid(950, 20) + 40 })
  assert.deepEqual([notePlace(s, note).x, notePlace(s, note).y], [was.x + 40, was.y + 40])
  await s.undo()
  // Without a Card there is nothing to keep to the grid: they land where dropped.
  await cv.moveSelection(new Set(), new Set([note]), 33, 47, 20)
  assert.deepEqual([notePlace(s, note).x, notePlace(s, note).y], [was.x + 33, was.y + 47])
  prefs.set('showGrid', false)
}
console.log('marquee behavior: passed')
prefs.set('showGrid', originalGrid)
