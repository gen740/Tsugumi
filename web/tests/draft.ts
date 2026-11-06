// A Draft has no ports to declare: its wires make them and take them away,
// and it is written on its Card.
import assert from 'node:assert/strict'
import type { ProjectSession as Session } from '../src/model/session'
import { get, parse, str } from '../src/shared/json'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { CardGeometry, NEW_PORT } = await import('../src/model/canvasKit')
const { key, ProjectSession } = await import('../src/model/session')

const session = () => {
  const s = new ProjectSession('/p', 'k', 'p')
  s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  s.layout = parse('{}')
  s.scheduleSave = () => {} // no tsugumid here
  return s
}
const wires = (s: Session) => s.connections([]).map((c) =>
  `${str(get(c, 'from', 'card'))}.${str(get(c, 'from', 'port'))}>${str(get(c, 'to', 'card'))}.${str(get(c, 'to', 'port'))}`)
const ports = (s: Session, k: string) => {
  const d = s.portData(k)
  return [d.inputs.map((p) => p.name), d.outputs.map((p) => p.name)]
}

{
  const s = session()
  const a = s.addCard('draft', [], { x: 0, y: 0 }), b = s.addCard('draft', [], { x: 300, y: 0 })
  assert.deepEqual(ports(s, a), [[], []], 'a new Draft has no ports')

  // Dropped anywhere on a Draft: both ends get a port.
  s.connect([a, NEW_PORT], [b, NEW_PORT], [])
  assert.deepEqual(wires(s), ['draft.out>draft-2.in'])
  assert.deepEqual(ports(s, a), [[], ['out']])
  assert.deepEqual(ports(s, b), [['in'], []])

  // Another wire: another port (no fork, nothing replaced).
  const c = s.addCard('draft', [], { x: 600, y: 0 })
  s.connect([a, NEW_PORT], [c, NEW_PORT], [])
  s.connect([c, NEW_PORT], [b, NEW_PORT], [])
  assert.deepEqual(ports(s, a), [[], ['out', 'out2']])
  assert.deepEqual(ports(s, b), [['in', 'in2'], []])
  assert.equal(s.connections([]).length, 3)

  // A port takes the name of the port at the other end when it says something.
  const e = s.addCard('input', [], { x: 0, y: 300 })
  const d = s.addCard('draft', [], { x: 300, y: 300 })
  s.connect([e, 'value'], [d, NEW_PORT], [])
  assert.deepEqual(ports(s, d), [['value'], []])

  // A port goes with its last wire; the other ports stay.
  s.removeConnection(0, [])
  assert.deepEqual(ports(s, a), [[], ['out2']])
  assert.deepEqual(ports(s, b), [['in2'], []])
  s.disconnect(c, false, [])
  assert.deepEqual(ports(s, b), [[], []])
  s.deleteCards(new Set([e]), [])
  assert.deepEqual(ports(s, d), [[], []])

  // A declared port without a wire is used before a new one is made.
  s.updateCard(d, (card) => card.set('inputs', [parse('{"name":"planned"}')]))
  s.connect([a, 'out2'], [d, NEW_PORT], [])
  assert.deepEqual(ports(s, d), [['planned'], []])
}

{
  const s = session()
  const a = s.addCard('draft', [], { x: 0, y: 0 })
  s.startDraftEdit(a)
  s.setDraftEdit({ content: 'count words' })
  const open = new CardGeometry(s.nodeData(a), 'topToBottom')
  assert.equal(open.draftRow, CardGeometry.draftEditRow, 'the Card opens up while it is written')
  assert.equal(await s.finishDraftEdit(), undefined)
  assert.equal(s.inlineDraft, undefined)
  assert.equal(str(get(s.cardMeta(a), 'content')), 'count words')
  assert.equal(s.nodeData(a).summary, 'count words', 'what a Draft says is on its Card')

  // No name is fine: the Card shows its content only, and keeps the id made for it.
  assert.ok(s.draftUntitled(a))
  assert.ok(s.nodeData(a).untitled)
  const plain = new CardGeometry(s.nodeData(a), 'topToBottom')
  assert.equal(plain.headerRow, CardGeometry.untitledTop)
  s.startDraftEdit(a)
  assert.equal(s.inlineDraft?.name, '', 'the name field is empty, not the made-up id')
  assert.equal(s.draftNameProblem(a, ''), undefined)
  assert.equal(await s.finishDraftEdit(), undefined)
  assert.ok(s.instance(a))

  // A name the Card cannot take is reported and not kept.
  assert.match(s.cardNameProblem(a, '日本語') ?? '', /英数字/)
  assert.equal(s.cardNameProblem(a, 'draft'), undefined)
  s.startDraftEdit(a)
  s.setDraftEdit({ name: '日本語' })
  assert.equal(await s.finishDraftEdit(), undefined)
  assert.match(s.notice ?? '', /英数字/)
  assert.ok(s.instance(key(['draft'])))
}

{
  // A side without a port starts its wire from the middle; a side with one from it; none while collapsed.
  const s = session()
  const a = s.addCard('draft', [], { x: 0, y: 0 }), b = s.addCard('draft', [], { x: 300, y: 0 })
  // The grid has its own test (portGrid.ts).
  const offGrid = (k: string) => ({ ...s.nodeData(k), portGrid: undefined })
  const empty = new CardGeometry(offGrid(a), 'topToBottom')
  assert.deepEqual(empty.newPort(true), { x: CardGeometry.width / 2, y: empty.height })
  assert.deepEqual(empty.port(NEW_PORT, false), { x: CardGeometry.width / 2, y: 0 })
  s.connect([a, NEW_PORT], [b, NEW_PORT], [])
  const wired = new CardGeometry(offGrid(a), 'topToBottom')
  assert.deepEqual(wired.port('out', true), { x: CardGeometry.width / 2, y: wired.height }, 'the port is where the wire started')
  assert.equal(wired.newPort(true), undefined, 'a side with a port starts its wires from it')
  // Across a left-to-right flow: the middle of the side; several around it.
  const side = new CardGeometry(offGrid(b), 'leftToRight')
  assert.deepEqual(side.port('in', false), { x: 0, y: side.height / 2 })
  const none = new CardGeometry({ ...offGrid(b), inputs: [] }, 'leftToRight')
  assert.deepEqual(none.newPort(false), { x: 0, y: none.height / 2 })
  const two = new CardGeometry({ ...offGrid(b), inputs: [{ name: 'x' }, { name: 'y' }] }, 'leftToRight')
  assert.equal((two.port('x', false).y + two.port('y', false).y) / 2, two.height / 2)
  assert.equal(empty.height, wired.height, 'wires do not change the size of a Draft')
  s.setCollapsed([b], true)
  assert.equal(new CardGeometry(offGrid(b), 'topToBottom').newPort(true), undefined)
}
console.log('draft behavior: passed')
