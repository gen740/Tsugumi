// A name a Card has had can be taken again: the new Card (the next Draft)
// starts without that Card's result and frame.
import assert from 'node:assert/strict'
import type { ProjectSession as Session } from '../src/model/session'
import { parse } from '../src/shared/json'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { Scene } = await import('../src/model/canvas')
const { ProjectSession } = await import('../src/model/session')

const session = (cards: string[]) => {
  const s = new ProjectSession('/p', 'k', 'p')
  s.project = parse(JSON.stringify({ name: 't', canvas: { cards: cards.map((id) => ({ id })), connections: [] } }))
  s.layout = parse('{}')
  s.scheduleSave = () => {} // no tsugumid here
  return s
}
const framed = (s: Session, k: string) => {
  s.updateDisplayFrame(k, (d) => {
    d.set('x', 700)
    d.set('y', 700)
    d.set('width', 300)
    d.set('height', 200)
  })
  s.displays.set(k, { runId: 'r', invocation: 1n, revision: 1n, value: { kind: 'text', text: 'old' } })
  s.cardStates.set(k, { state: 2, invocation: 1n, stages: [], cached: false, workerReused: false, sent: new Set(), sentInRun: new Set() })
}
// What the core says is in the document after a save.
const resolved = (s: Session, cards: string[]) =>
  (s as unknown as { applyResolution(r: unknown[], d: unknown[]): void }).applyResolution(
    cards.map((id) => ({ card: { segments: [id] }, inputs: [], outputs: [], diagnostics: [], nixReferences: [] })), [])
const clean = (s: Session, k: string) => {
  assert.equal(s.displays.has(k), false, 'no result of the Card that had the name')
  assert.equal(s.displayFrame(k), undefined, 'no frame of the Card that had the name')
  assert.equal(s.nodeData(k).hasDisplay, false)
  assert.equal(s.cardStates.has(k), false, 'no run state of the Card that had the name')
}

{
  // A Card goes (a delete, a rename, the Agent's save): its result goes with it.
  const s = session(['b', 'other'])
  resolved(s, ['b', 'other'])
  framed(s, 'b')
  framed(s, 'other')
  resolved(s, ['other'])
  assert.equal(s.displays.has('b'), false)
  assert.equal(s.displays.has('other'), true, 'the other Cards keep theirs')
}

{
  // A frame left under the name (a Card in Card's layout, a file written by hand). The next Draft takes the name.
  const s = session(['renamed'])
  resolved(s, ['b'])
  framed(s, 'b')
  resolved(s, ['renamed'])
  const k = s.addCard('draft', [], { x: 0, y: 0 }, 'b')
  assert.equal(k, 'b')
  clean(s, k)
  // Undone: what the layout had is back, the Card gone.
  s.undo()
  assert.equal(s.instance('b'), undefined)
  assert.notEqual(s.displayFrame('b'), undefined)
}

{
  // Before the core has answered: the result is still here when the Card comes.
  const s = session([])
  resolved(s, ['draft'])
  framed(s, 'draft')
  const k = s.addCard('draft', [], { x: 0, y: 0 })
  assert.equal(k, 'draft')
  clean(s, k)
  assert.equal(s.nodeData(k).result, undefined)
}

{
  // A Draft under a name free of all that is left alone, and so is what others have.
  const s = session(['a'])
  resolved(s, ['a'])
  framed(s, 'a')
  const k = s.addCard('draft', [], { x: 0, y: 0 })
  assert.equal(k, 'draft')
  clean(s, k)
  assert.equal(s.displays.has('a'), true)
  assert.notEqual(s.displayFrame('a'), undefined)
}

{
  // Copies of Cards start clean too.
  const s = session([])
  const a = s.addCard('logger', [], { x: 0, y: 0 }, 'a')
  framed(s, 'a-2')
  const made = await s.duplicate(new Set([a]), [])
  assert.deepEqual([...made], ['a-2'])
  clean(s, 'a-2')
}
{
  // Put where another Card is, as E does: it goes to a free place, without the old frame.
  const s = session([])
  const here = { x: 4000, y: 4000 }
  const occupied = s.addCard('draft', [], here, 'occupied')
  resolved(s, ['b', 'occupied'])
  framed(s, 'b')
  const k = s.addCard('draft', [], here, 'b', (c) => new Scene(s, []).freeSpot(here, c))
  assert.equal(k, 'b')
  clean(s, k)
  const scene = new Scene(s, [])
  const a = scene.cardRect(k), b = scene.cardRect(occupied)
  assert.ok(!(a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height), 'a free place')
}
console.log('freed name behavior: passed')
