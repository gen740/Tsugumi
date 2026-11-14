// What the core refuses to save: Arrange does not make such a layout, Discard Changes goes
// back to what is saved, and the state of the save follows.
import assert from 'node:assert/strict'
import { parse, get, dbl } from '../src/shared/json'
import { RPCFailure } from '../src/shared/rpc'
import * as D from '../src/gen/tsugumi/data'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')

const session = () => {
  const s = new ProjectSession('/p', 'k', 'p')
  s.project = parse('{"name":"t","canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}}')
  s.confirmedProject = parse('{"name":"t","canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}}')
  s.layout = parse('{}')
  s.confirmedLayout = parse('{}')
  s.confirmedDefinitions.set('a', { card: parse('{"language":"bash","inputs":[{"name":"in"}]}') })
  s.confirmedDefinitions.set('b', { card: parse('{"language":"bash","inputs":[{"name":"in"}]}') })
  s.scheduleSave = () => {} // saved by hand below
  s.connected = true
  return s
}
const saveOnce = (s: unknown) => (s as { saveOnce(): Promise<void> }).saveOnce()
const refused = new RPCFailure({ kind: D.FailureKind.invalid_document, diagnostic: 'layout.json: display b needs x, y, width and height' })

// Arrange gives a frame still drawn beside its Card the whole frame the core asks for.
{
  const s = session()
  s.displays.set('b', { runId: 'r', invocation: 1n, revision: 1n, value: { kind: 'text', text: 'shown' } })
  assert.equal(s.displayFrame('b'), undefined, 'shown beside its Card, with no frame in the layout')
  s.client = { layoutArrange: async () => ({ cards: [{ id: 'a', x: 0, y: 0 }, { id: 'b', x: 300, y: 0 }], frames: [{ id: 'b', x: 600, y: 0 }], notes: [], portOrders: [] }) } as never
  await s.arrange([])
  const f = s.displayFrame('b')
  assert.notEqual(dbl(get(f, 'x')), undefined)
  assert.notEqual(dbl(get(f, 'y')), undefined)
  assert.equal(dbl(get(f, 'width')), 320)
  assert.equal(dbl(get(f, 'height')), 170)
}

// A refused save, then Discard Changes: back to what is saved, and the next edit is saved.
{
  const s = session()
  let refuse = true
  const sent: unknown[] = []
  s.client = {
    save: async (_rev: unknown, _op: unknown, _origin: unknown, changes: unknown[]) => {
      if (refuse) throw refused
      sent.push(changes)
      return { rev: 1n }
    },
  } as never
  s.edit((x) => x.setPosition('a', { x: 10, y: 20 }))
  await saveOnce(s)
  assert.ok(s.saveError?.includes('display b'), s.saveError)
  assert.ok(s.dirty)

  s.discardDraft()
  assert.ok(!s.dirty, 'nothing is left of the draft')
  assert.equal(s.saveError, undefined)
  assert.equal(s.syncState.kind, 'saved')
  assert.notDeepEqual(s.position('a'), { x: 10, y: 20 })

  refuse = false
  s.edit((x) => x.setPosition('a', { x: 30, y: 40 }))
  await saveOnce(s)
  assert.equal(s.saveError, undefined, s.saveError)
  assert.equal(sent.length, 1, 'saving works again')
}

// The refused edit undone: nothing is left to save, so the error does not stay.
{
  const s = session()
  s.client = { save: async () => { throw refused } } as never
  s.edit((x) => x.setPosition('a', { x: 10, y: 20 }))
  await saveOnce(s)
  assert.ok(s.saveError)
  s.undo()
  assert.ok(!s.dirty)
  await saveOnce(s)
  assert.equal(s.saveError, undefined, 'the state of the save follows')
  assert.equal(s.syncState.kind, 'saved')
}

// A save on its way when the draft is discarded: its answer is about nothing now.
for (const outcome of ['refused', 'saved']) {
  const s = session()
  Reflect.deleteProperty(s, 'scheduleSave')
  s.connected = true
  let answer: (() => void) | undefined
  s.client = {
    save: () => new Promise((done, reject) => {
      answer = () => outcome === 'refused' ? reject(refused) : done({ rev: 1n })
    }),
  } as never
  s.edit((x) => x.setPosition('a', { x: 10, y: 20 }))
  for (let i = 0; i < 50 && !answer; i++) await new Promise((r) => setTimeout(r, 1))
  assert.ok(answer, 'the save is on its way')
  s.discardDraft()
  const saving = (s as unknown as { saveTask: Promise<void> }).saveTask
  answer!()
  await saving
  await new Promise((r) => setTimeout(r, 1))
  assert.ok(!s.dirty, outcome)
  assert.equal(s.saveError, undefined, s.saveError)
  assert.equal(s.syncState.kind, 'saved', outcome)
}

// Discard Changes drops a name edit without changing the Card identifier.
{
  const s = session()
  s.edit((x) => x.updateMeta('a', (m) => m.set('name', 'c')))
  assert.equal(s.displayName('a'), 'c')
  s.discardDraft()
  assert.equal(s.displayName('a'), 'a')
  assert.ok(s.instance('a'))
}

// Home with what cannot be saved: the user is told why and may give it up.
{
  const s = session()
  s.client = { save: async () => { throw refused } } as never
  s.edit((x) => x.setPosition('a', { x: 10, y: 20 }))
  const asked: string[] = []
  // (Saved by hand: no tsugumid connected here.)
  s.save = () => saveOnce(s)
  assert.equal(await s.settleBeforeLeaving(async (reason) => { asked.push(reason); return false }), false, 'staying')
  assert.ok(asked[0]?.includes('display b'), 'it says why')
  assert.ok(s.dirty, 'and the changes stay')
  assert.equal(await s.settleBeforeLeaving(async () => true), true, 'giving them up')
  assert.ok(!s.dirty)
  assert.equal(s.syncState.kind, 'saved')
  // Nothing to give up: the project is left without a question.
  assert.equal(await s.settleBeforeLeaving(async () => assert.fail('nothing to ask')), true)
}
console.log('save failure behavior: passed')
