import { addCodeCard, cardIds } from './cardIds'
// A name a Card has had can be taken again: the new Card (the next Draft)
// starts without that Card's result.
import assert from 'node:assert/strict'
import type { ProjectSession as Session } from '../src/model/session'
import { parse } from '../shared/json'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')

const session = (cards: string[]) => {
  const s = cardIds(new ProjectSession('/p', 'k', 'p'))
  s.project = parse(JSON.stringify({ name: 't', canvas: { cards: cards.map((id) => ({ id })), connections: [] } }))
  s.layout = parse('{}')
  return s
}
const rememberResult = (s: Session, k: string) => {
  s.liveViews.observe(k, () => {})
  const views = s.liveViews.get(k)!
  views.declare([{ name: 'chart', type: 'html', blob: '/nix/store/chart' }])
  views.data('chart', Uint8Array.of(42))
  s.cardStates.set(k, { state: 2, invocation: 1n, stages: [], cached: false, workerReused: false, sent: new Set(), sentInRun: new Set() })
}
// What the core says is in the document after a save.
const resolved = (s: Session, cards: string[]) =>
  (s as unknown as { applyResolution(r: unknown[], d: unknown[]): void }).applyResolution(
    cards.map((id) => ({ card: { segments: [id] }, inputs: [], outputs: [], diagnostics: [], nixReferences: [] })), [])
const clean = (s: Session, k: string) => {
  assert.equal(!!s.liveViews.get(k)?.views.length, false, 'no result of the Card that had the name')
  assert.equal(s.cardStates.has(k), false, 'no run state of the Card that had the name')
}

{
  // A Card goes (a delete, a rename, the Agent's save): its result goes with it.
  const s = session(['b', 'other'])
  resolved(s, ['b', 'other'])
  rememberResult(s, 'b')
  rememberResult(s, 'other')
  resolved(s, ['other'])
  assert.equal(!!s.liveViews.get('b')?.views.length, false)
  assert.equal(!!s.liveViews.get('other')?.views.length, true, 'the other Cards keep theirs')
}

{
  // A deleted Card leaves its name available to the next Draft.
  const s = session(['renamed'])
  resolved(s, ['b'])
  rememberResult(s, 'b')
  resolved(s, ['renamed'])
  const k = await s.addCard('draft', [], { x: 0, y: 0 }, 'b')
  assert.equal(k, 'b')
  clean(s, k)
  // Undo removes the new Card.
  await s.undo()
  assert.equal(s.instance('b'), undefined, s.saveError)
}

{
  // Before the core has answered: the result is still here when the Card comes.
  const s = session([])
  resolved(s, ['draft'])
  rememberResult(s, 'draft')
  const k = await s.addCard('draft', [], { x: 0, y: 0 })
  assert.equal(k, 'draft')
  clean(s, k)
  assert.equal(s.nodeData(k).result, undefined)
}

{
  // A Draft under a name free of all that is left alone, and so is what others have.
  const s = session(['a'])
  resolved(s, ['a'])
  rememberResult(s, 'a')
  const k = await s.addCard('draft', [], { x: 0, y: 0 })
  assert.equal(k, 'draft')
  clean(s, k)
  assert.equal(!!s.liveViews.get('a')?.views.length, true)
}

{
  // Copies of Cards start clean too.
  const s = session([])
  const a = await addCodeCard(s, { x: 0, y: 0 }, 'a')
  rememberResult(s, 'a_2')
  const made = await s.duplicate(new Set([a]), [])
  assert.deepEqual([...made], ['a_2'])
  clean(s, 'a_2')
}
{
  // Put where another Card is, as E does: it goes to a free place.
  const s = session([])
  const here = { x: 4000, y: 4000 }
  const occupied = await s.addCard('draft', [], here, 'occupied')
  resolved(s, ['b', 'occupied'])
  rememberResult(s, 'b')
  const k = await s.addCard('draft', [], here, 'b', true)
  assert.equal(k, 'b')
  clean(s, k)
  assert.notDeepEqual(s.position(k), here, 'the host places the new Card away from the occupied position')
  assert.deepEqual(s.position(occupied), here)
}
console.log('freed name behavior: passed')
