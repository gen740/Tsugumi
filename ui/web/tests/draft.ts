import { projectFixture } from './documentFixture'
import { R } from '../src/wire/rpc'
import { cardIds } from './cardIds'
// A Draft has no ports: its wires attach directly to the Card,
// and it is written on its Card.
import assert from 'node:assert/strict'
import type { ProjectSession as Session } from '../src/model/session'
import { get, parse, serialize, str } from '../shared/json'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { CardGeometry, NEW_PORT } = await import('../src/model/canvasKit')
const { ProjectSession } = await import('../src/model/session')

const session = () => {
  const s = cardIds(new ProjectSession('/p', 'k', 'p'))
  s.project = parse(projectFixture({"name":"t","canvas":{"cards":[],"connections":[]}}))
  s.layout = parse('{}')
  s.connected = true
  return s
}
const ports = (s: Session, k: string) => {
  const d = s.portData(k)
  return [d.inputs.map((p) => p.name), d.outputs.map((p) => p.name)]
}

{
  const s = session()
  const a = await s.addCard('draft', [], { x: 0, y: 0 }), b = await s.addCard('draft', [], { x: 300, y: 0 })
  assert.deepEqual(ports(s, a), [[], []], 'a new Draft has no ports')

  // Dropped anywhere on a Draft: both ends name the Card itself.
  await s.connect([a, NEW_PORT], [b, NEW_PORT], [])
  assert.deepEqual(s.connections([]).map(c => JSON.parse(serialize(c))), [{ from: { card: 'draft' }, to: { card: 'draft_2' } }])
  assert.equal(s.nodeData(a).symbol, 'pencil.and.outline')
  assert.deepEqual(s.layoutScene([]).wires, [{ from: a, fromPort: NEW_PORT, to: b, toPort: NEW_PORT }])
  assert.deepEqual(ports(s, a), [[], []])
  assert.deepEqual(ports(s, b), [[], []])

  // More wires attach to the same Card without replacing existing wires.
  const c = await s.addCard('draft', [], { x: 600, y: 0 })
  await s.connect([a, NEW_PORT], [c, NEW_PORT], [])
  await s.connect([c, NEW_PORT], [b, NEW_PORT], [])
  assert.deepEqual(ports(s, a), [[], []])
  assert.deepEqual(ports(s, b), [[], []])
  assert.equal(s.connections([]).length, 3)
  assert.equal(get(s.cardDefinition(a), 'inputs'), undefined)
  assert.equal(get(s.cardMeta(a), 'outputs'), undefined)

  // A real port name is never copied onto the Draft.
  const e = await s.addCard('input', [], { x: 0, y: 300 })
  const d = await s.addCard('draft', [], { x: 300, y: 300 })
  await s.connect([e, 'value'], [d, NEW_PORT], [])
  assert.deepEqual(ports(s, d), [[], []])

  // Removing wires leaves definitions and metadata untouched.
  s.removeConnection(0, [])
  assert.deepEqual(ports(s, a), [[], []])
  assert.deepEqual(ports(s, b), [[], []])
  s.disconnect(c, false, [])
  assert.deepEqual(ports(s, b), [[], []])
  s.deleteCards(new Set([e]), [])
  await s.save()
  assert.deepEqual(ports(s, d), [[], []])


}

{
  const s = session()
  const a = await s.addCard('draft', [], { x: 0, y: 0 })
  await s.save()
  await s.startDraftEdit(a)
  s.setDraftEdit({ content: 'count words' })
  const open = new CardGeometry(s.nodeData(a))
  assert.equal(open.draftRow, CardGeometry.draftEditRow, 'the Card opens up while it is written')
  await s.finishDraftEdit()
  assert.equal(s.inlineDraft, undefined)
  assert.equal(str(get(s.cardMeta(a), 'content')), 'count words')
  assert.equal(s.nodeData(a).summary, 'count words', 'what a Draft says is on its Card')

  assert.equal(s.nodeData(a).title, s.displayName(a))
  assert.equal(new CardGeometry(s.nodeData(a)).headerRow, CardGeometry.header)
  await s.save()
  await s.startDraftEdit(a)
  await s.finishDraftEdit()
  assert.ok(s.instance(a))
  assert.equal(s.displayName(a), str(get(s.cardMeta(a), 'title')))

}

{
  // Draft wires use the middle of each side; attachments hide while collapsed.
  const s = session()
  const a = await s.addCard('draft', [], { x: 0, y: 0 }), b = await s.addCard('draft', [], { x: 300, y: 0 })
  // The grid has its own test (portGrid.ts).
  const offGrid = (k: string) => ({ ...s.nodeData(k), portGrid: undefined })
  const empty = new CardGeometry(offGrid(a))
  assert.deepEqual(empty.newPort(true), { x: CardGeometry.width / 2, y: empty.height })
  assert.deepEqual(empty.port(NEW_PORT, false), { x: CardGeometry.width / 2, y: 0 })
  await s.connect([a, NEW_PORT], [b, NEW_PORT], [])
  const wired = new CardGeometry(offGrid(a))
  assert.deepEqual(wired.port(NEW_PORT, true), { x: CardGeometry.width / 2, y: wired.height }, 'the port is where the wire started')
  assert.deepEqual(wired.newPort(true), empty.newPort(true), 'the Card attachment stays available for more wires')
  const none = new CardGeometry({ ...offGrid(b), inputs: [] })
  assert.deepEqual(none.newPort(false), { x: CardGeometry.width / 2, y: 0 })
  // Supplied port declarations do not create Draft ports or move the anchor.
  const two = new CardGeometry({ ...offGrid(b), inputs: [{ name: 'x' }, { name: 'y' }] })
  assert.deepEqual(two.port(NEW_PORT, false), { x: CardGeometry.width / 2, y: 0 })
  assert.equal(empty.height, wired.height, 'wires do not change the size of a Draft')
  s.setCollapsed([b], true)
  await s.save()
  assert.equal(new CardGeometry(offGrid(b)).newPort(true), undefined)
}
console.log('draft behavior: passed')
