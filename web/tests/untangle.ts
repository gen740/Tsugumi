import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { crossingPairs, orderedPorts, PortOrder, UNTANGLE_TRIALS, untanglePorts, UntangleCard } from '../src/model/untangle'
import { Rect, WireRouter } from '../src/model/canvasKit'
import { arrangeCards, ArrangeInput, ArrangeWire } from '../src/model/arrange'
import { FlowDirection } from '../src/model/prefs'
import { ProjectSession } from '../src/model/session'
import { get, parse, serialize, str } from '../src/shared/json'
import { D } from '../src/shared/rpc'
import { CanvasController, Scene } from '../src/model/canvas'
import { WorkflowTab } from '../src/model/workspace'

const card = (id: string, x: number, y: number, inputs: string[], outputs: string[], movable = true): UntangleCard => ({
  id, x, y, width: 240, height: 80, movable,
  inputs: inputs.map((name, i) => ({ name, x: 240 * (i + 1) / (inputs.length + 1), y: 0 })),
  outputs: outputs.map((name, i) => ({ name, x: 240 * (i + 1) / (outputs.length + 1), y: 80 })),
})
interface Case { name: string; cards: UntangleCard[]; wires: ArrangeWire[]; extra?: Rect[] }
const cases: Case[] = [
  { name: 'adjacent outputs to Draft and code', cards: [card('source', 200, 0, [], ['draft', 'code']), card('left', 0, 300, ['in'], []), card('right', 420, 300, ['in'], [])],
    wires: [{ from: 'source', fromPort: 'draft', to: 'right', toPort: 'in' }, { from: 'source', fromPort: 'code', to: 'left', toPort: 'in' }] },
  { name: 'crossed inputs', cards: [card('left', 0, 0, [], ['out']), card('right', 420, 0, [], ['out']), card('sink', 200, 300, ['a', 'b'], [])],
    wires: [{ from: 'left', fromPort: 'out', to: 'sink', toPort: 'b' }, { from: 'right', fromPort: 'out', to: 'sink', toPort: 'a' }] },
  { name: 'fixed routing ports', cards: [card('branch', 200, 0, [], ['a', 'b'], false), card('sink', 200, 300, ['a', 'b'], [], false)],
    wires: [{ from: 'branch', fromPort: 'a', to: 'sink', toPort: 'b' }, { from: 'branch', fromPort: 'b', to: 'sink', toPort: 'a' }] },
  { name: 'multiple Cards and connections', cards: [card('a', 100, 0, [], ['one', 'unused', 'two']), card('b', 900, 0, [], ['one', 'two']),
    card('c', 0, 300, ['in'], []), card('d', 400, 300, ['in'], []), card('e', 800, 300, ['in'], []), card('f', 1200, 300, ['in'], [])],
    wires: [{ from: 'a', fromPort: 'one', to: 'd', toPort: 'in' }, { from: 'a', fromPort: 'two', to: 'c', toPort: 'in' },
      { from: 'b', fromPort: 'one', to: 'f', toPort: 'in' }, { from: 'b', fromPort: 'two', to: 'e', toPort: 'in' }] },
  { name: 'fixed boundary bar', cards: [card('boundary', 0, 0, [], ['a', 'b'], false), card('inner', 0, 300, ['first', 'second'], [])],
    wires: [{ from: 'boundary', fromPort: 'a', to: 'inner', toPort: 'second' }, { from: 'boundary', fromPort: 'b', to: 'inner', toPort: 'first' }] },
]
cases.push({ name: 'reduce crossings with fixed ports remaining', cards: [...cases[0].cards,
  card('fixed-source', 1000, 0, [], ['a', 'b'], false), card('fixed-sink', 1000, 300, ['a', 'b'], [], false)],
  wires: [...cases[0].wires, { from: 'fixed-source', fromPort: 'a', to: 'fixed-sink', toPort: 'b' }, { from: 'fixed-source', fromPort: 'b', to: 'fixed-sink', toPort: 'a' }] })
// One port wired to both sides has no better slot: either order crosses once, so nothing moves.
cases.push({ name: 'fan-out to both sides', cards: [card('source', 400, 0, [], ['wide', 'near']), card('left', 0, 300, ['in'], []), card('middle', 200, 600, ['in'], []), card('right', 800, 300, ['in'], [])],
  wires: [{ from: 'source', fromPort: 'wide', to: 'left', toPort: 'in' }, { from: 'source', fromPort: 'wide', to: 'right', toPort: 'in' }, { from: 'source', fromPort: 'near', to: 'middle', toPort: 'in' }] })

/** Layered graphs with tangled wires, the same for a seed everywhere. */
function tangle(seed: number, layers: number, per: number, fan: number, gap = 260): { cards: UntangleCard[]; wires: ArrangeWire[] } {
  let state = seed >>> 0
  const r = () => (state = (Math.imul(state, 1664525) + 1013904223) >>> 0) / 4294967296
  const cards: UntangleCard[] = [], wires: ArrangeWire[] = []
  for (let l = 0; l < layers; l++) for (let i = 0; i < per; i++) {
    const ins = l === 0 ? 0 : 1 + Math.floor(r() * fan), outs = l === layers - 1 ? 0 : 1 + Math.floor(r() * fan)
    const x = i * 320 + Math.floor(r() * 4) * 20, movable = r() > 0.1
    cards.push(card(`c${l}_${i}`, x, l * gap, Array.from({ length: ins }, (_, k) => 'i' + k), Array.from({ length: outs }, (_, k) => 'o' + k), movable))
  }
  for (let l = 1; l < layers; l++) for (let i = 0; i < per; i++) {
    const to = cards[l * per + i]
    for (const p of to.inputs) {
      const from = cards[(l - 1) * per + Math.min(per - 1, Math.max(0, i + Math.floor(r() * 5) - 2))]
      wires.push({ from: from.id, fromPort: from.outputs[Math.floor(r() * from.outputs.length)].name, to: to.id, toPort: p.name })
    }
  }
  return { cards, wires }
}
const free = (g: { cards: UntangleCard[]; wires: ArrangeWire[] }) => ({ ...g, cards: g.cards.map(c => ({ ...c, movable: true })) })
// Taking every proposal together leaves this graph as it was (3 crossings): one
// Card's new order removes a crossing and another's adds one. Card by Card, the first is kept.
cases.push({ name: 'one Card helps while another would get worse', ...free(tangle(2971, 3, 3, 3)) })
// The frame bends the wires around it; the order that is best without it is not best with it.
cases.push({ name: 'an output frame in the way', ...free(tangle(7, 2, 3, 3, 420)), extra: [{ x: 140, y: 200, width: 320, height: 160 }] })
for (const seed of [11, 12, 13, 14]) cases.push({ name: 'tangle ' + seed, ...tangle(seed, 3 + seed % 3, 3 + seed % 4, 3) })

function count(flow: FlowDirection, cs: UntangleCard[], ws: ArrangeWire[], extra: Rect[] = []) {
  const ends = ws.map(w => {
    const a = cs.find(c => c.id === w.from)!, b = cs.find(c => c.id === w.to)!
    const p = a.outputs.find(p => p.name === w.fromPort)!, q = b.inputs.find(p => p.name === w.toPort)!
    return { a: { x: a.x + p.x, y: a.y + p.y }, b: { x: b.x + q.x, y: b.y + q.y } }
  })
  return crossingPairs(new WireRouter(flow, [...cs, ...extra]).routeAll(ends).map(r => r.pts), ends)
}
const reordered = (cards: UntangleCard[], orders: PortOrder[]) => cards.map(c => {
  const o = orders.find(o => o.id === c.id)
  return !o ? c : { ...c, inputs: c.inputs.map((p, i) => ({ ...p, name: o.inputs[i] })), outputs: c.outputs.map((p, i) => ({ ...p, name: o.outputs[i] })) }
})
/** The same graph for the other flow direction. */
const turned = (cards: UntangleCard[]) => cards.map(c => ({ ...c, x: c.y, y: c.x, width: c.height, height: c.width,
  inputs: c.inputs.map(p => ({ name: p.name, x: p.y, y: p.x })), outputs: c.outputs.map(p => ({ name: p.name, x: p.y, y: p.x })) }))
const turnedRects = (rects: Rect[]) => rects.map(r => ({ x: r.y, y: r.x, width: r.height, height: r.width }))
/** Crossings before and after, where a case is about exact numbers (both flow directions). */
const expected: Record<string, [number, number]> = {
  'adjacent outputs to Draft and code': [1, 0], 'crossed inputs': [1, 0], 'fixed routing ports': [1, 1], 'multiple Cards and connections': [2, 0],
  'fixed boundary bar': [1, 0], 'reduce crossings with fixed ports remaining': [2, 1], 'fan-out to both sides': [1, 1],
}
const fixtures: unknown[] = []
for (const c of cases) for (const flow of ['topToBottom', 'leftToRight'] as const) {
  const cards = flow === 'topToBottom' ? c.cards : turned(c.cards)
  const extra = c.extra && (flow === 'topToBottom' ? c.extra : turnedRects(c.extra))
  const before = JSON.stringify(cards)
  const orders = untanglePorts(flow, cards, c.wires, extra)
  assert.equal(JSON.stringify(cards), before, 'input is not mutated')
  const after = reordered(cards, orders)
  const was = count(flow, cards, c.wires, extra), now = count(flow, after, c.wires, extra)
  if (expected[c.name]) assert.deepEqual([was, now], expected[c.name], `${c.name} ${flow}: crossings before and after`)
  assert.ok(orders.length ? now < was : now === was, `${c.name} ${flow}: ports move only for fewer crossings`)
  if (c.name === 'multiple Cards and connections') assert.equal(after[0].outputs[1].name, 'unused', 'unconnected ports retain their slots')
  if (c.name === 'fan-out to both sides') assert.deepEqual(orders, [], 'no order is better, so none is saved')
  if (c.name.startsWith('one Card helps')) assert.ok(now < was, `${c.name} ${flow}: the Card that helps is not held back`)
  if (c.extra) {
    const unaware = reordered(cards, untanglePorts(flow, cards, c.wires))
    const blind = count(flow, unaware, c.wires, extra)
    // The router is not the same turned on its side: there the two orders may tie.
    assert.ok(flow === 'topToBottom' ? now < blind : now <= blind, `${c.name} ${flow}: the frame decides between orders`)
  }
  assert.ok(cards.every((card, i) => card.movable || JSON.stringify(after[i]) === JSON.stringify(card)), `${c.name}: fixed ports stay`)
  assert.deepEqual(untanglePorts(flow, after, c.wires, extra), [], 'running twice does not move more ports')
  fixtures.push({ name: c.name, flow, cards, wires: c.wires, ...(extra ? { extra } : {}), expect: orders })
}
const fixturePath = path.join(process.argv[2], 'tests/fixtures/untangle-contract.json')
if (process.argv.includes('--update-untangle')) fs.writeFileSync(fixturePath, '[\n' + fixtures.map(c => JSON.stringify(c)).join(',\n') + '\n]\n')
assert.deepEqual(JSON.parse(fs.readFileSync(fixturePath, 'utf8')), fixtures, 'Mac and Web share the same cases (npm test -- --update-untangle)')
assert.deepEqual(orderedPorts([{ name: 'a' }, { name: 'b' }, { name: 'new' }], ['gone', 'b', 'b', 'a']).map(p => p.name), ['b', 'a', 'new'])

// Any graph: never more crossings than before, and a second run has nothing left to do.
for (let seed = 1; seed <= 60; seed++) {
  const flow: FlowDirection = seed % 2 ? 'topToBottom' : 'leftToRight'
  const g = tangle(seed, 3 + seed % 3, 3 + seed % 4, 3)
  const cards = flow === 'topToBottom' ? g.cards : turned(g.cards)
  const orders = untanglePorts(flow, cards, g.wires), after = reordered(cards, orders)
  assert.ok(count(flow, after, g.wires) <= count(flow, cards, g.wires), `tangle ${seed}: crossings do not grow`)
  assert.ok(orders.every(o => cards.find(c => c.id === o.id)!.movable), `tangle ${seed}: only movable ports`)
  for (const o of orders) {
    const c = cards.find(c => c.id === o.id)!
    assert.deepEqual([...o.inputs].sort(), c.inputs.map(p => p.name).sort(), `tangle ${seed}: the same inputs`)
    assert.deepEqual([...o.outputs].sort(), c.outputs.map(p => p.name).sort(), `tangle ${seed}: the same outputs`)
  }
  assert.deepEqual(untanglePorts(flow, after, g.wires), [], `tangle ${seed}: a second run changes nothing`)
}

// A few hundred Cards: the work is bounded by the number of trials, not by the canvas.
{
  const g = tangle(7, 15, 20, 3)
  const routeAll = WireRouter.prototype.routeAll
  let runs = 0
  WireRouter.prototype.routeAll = function (this: WireRouter, ...args: Parameters<typeof routeAll>) { runs++; return routeAll.apply(this, args) }
  const started = performance.now()
  let orders: PortOrder[]
  try { orders = untanglePorts('topToBottom', g.cards, g.wires) } finally { WireRouter.prototype.routeAll = routeAll }
  assert.equal(g.cards.length, 300)
  assert.ok(runs <= UNTANGLE_TRIALS + 1, `at most ${UNTANGLE_TRIALS + 1} router runs for 300 Cards (${runs})`)
  assert.ok(performance.now() - started < 20000, 'a large canvas stays within an interactive budget')
  assert.ok(count('topToBottom', reordered(g.cards, orders), g.wires) < count('topToBottom', g.cards, g.wires), 'and it still removes crossings')
}

// Arrange, untangle, then arrange again for the new port order, as `ProjectSession.arrange`
// does: Cards never overlap, crossings never grow, and a straight wire is given up only
// for a crossing removed.
{
  type Placed = ReturnType<typeof arrangeCards>
  const contract: { name: string; input: ArrangeInput; expect: Placed }[] = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'tests/fixtures/arrange-contract.json'), 'utf8'))
  let untangled = 0
  for (const c of contract) {
    const flow = c.input.flow
    const place = (input: ArrangeInput, placed: Placed): UntangleCard[] => input.cards.map(n => ({ ...n, ...placed.cards.find(e => e.id === n.id)!, movable: true }))
    const frames = (placed: Placed) => placed.frames.map(f => ({ ...c.input.frames.find(g => g.id === f.id)!, x: f.x, y: f.y }))
    const straight = (cs: UntangleCard[]) => c.input.wires.filter(w => {
      const a = cs.find(x => x.id === w.from), b = cs.find(x => x.id === w.to)
      const p = a?.outputs.find(p => p.name === w.fromPort), q = b?.inputs.find(p => p.name === w.toPort)
      return a && b && p && q && (flow === 'topToBottom' ? a.x + p.x === b.x + q.x : a.y + p.y === b.y + q.y)
    }).length
    const first = place(c.input, c.expect)
    const orders = untanglePorts(flow, first, c.input.wires, frames(c.expect))
    if (!orders.length) continue
    untangled++
    const ports = reordered(first, orders)
    const again: ArrangeInput = { ...c.input, cards: c.input.cards.map(n => { const m = ports.find(x => x.id === n.id)!; return { ...n, inputs: m.inputs, outputs: m.outputs } }) }
    const placed = arrangeCards(again), last = place(again, placed)
    const boxes = [...last, ...frames(placed)]
    for (let i = 0; i < boxes.length; i++) for (let j = i + 1; j < boxes.length; j++) {
      const a = boxes[i], b = boxes[j]
      assert.ok(a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y || b.y + b.height <= a.y, `${c.name}: nothing overlaps after untangling`)
    }
    const c0 = count(flow, first, c.input.wires, frames(c.expect)), c1 = count(flow, ports, c.input.wires, frames(c.expect)), c2 = count(flow, last, c.input.wires, frames(placed))
    assert.ok(c1 < c0 && c2 <= c1, `${c.name}: crossings ${c0} -> ${c1} -> ${c2} do not grow`)
    assert.ok(straight(first) - straight(last) <= c0 - c2, `${c.name}: a straight wire is given up only for a crossing removed`)
    assert.deepEqual(untanglePorts(flow, last, c.input.wires, frames(placed)), [], `${c.name}: placing again leaves nothing to untangle`)
    if (c.name.startsWith('crossing-ports') && c.name.endsWith('grid20')) assert.equal(straight(last), straight(first), `${c.name}: the straight wires are straight again`)
  }
  assert.ok(untangled > 10, 'the demos have wires to untangle')
}

const s = new ProjectSession('/untangle', 'untangle', 'untangle')
s.project = parse('{"name":"untangle","canvas":{"cards":[],"connections":[]}}')
s.layout = parse('{"flow":"topToBottom"}')
s.scheduleSave = () => {}
for (const [id, x, y, ins, outs] of [ ['source', 200, 0, [], ['a', 'b']], ['left', 0, 300, ['in'], []], ['right', 500, 300, ['in'], []] ] as [string, number, number, string[], string[]][]) {
  s.addCard(id === 'right' ? 'draft' : 'python', [], { x, y }, id)
  const ports = (names: string[]) => names.map(name => ({ name, kind: D.PortKind.value, optional: false }))
  s.resolution.set(id, { path: [id], inputs: ports(ins), outputs: ports(outs), diagnostics: [] })
}
s.updateCard('right', d => d.set('inputs', parse('[{"name":"in"}]')))
s.connect(['source', 'a'], ['right', 'in'], [])
s.connect(['source', 'b'], ['left', 'in'], [])
const sceneCrossings = (scope: string[] = []) => {
  const paths = new Scene(s, scope).wires.filter(w => w.style !== 'display').map(w => w.pts)
  return crossingPairs(paths, paths.map(p => ({ a: p[0], b: p[p.length - 1] })))
}
const styles = () => new Scene(s, []).wires.map(w => !!w.dotted)
assert.deepEqual(styles(), [true, false], 'the adjacent wires are dotted and solid')
assert.equal(sceneCrossings(), 1)
const projectBefore = serialize(s.project), layoutBefore = serialize(s.layout), definitionsBefore = JSON.stringify([...s.definitionDrafts])
const steps = s.undoStack.length
s.untangle([])
assert.equal(sceneCrossings(), 0, 'the actual Scene removes the crossing')
assert.deepEqual(styles(), [true, false], 'dots stay with the named Draft wire')
assert.deepEqual(s.geometry('source').outputs, ['b', 'a'])
assert.equal(serialize(s.project), projectBefore, 'named endpoints are unchanged')
assert.equal(JSON.stringify([...s.definitionDrafts]), definitionsBefore, 'schemas and params are unchanged')
assert.deepEqual(s.position('source'), { x: 200, y: 0 }, 'Card positions stay')
assert.equal(s.undoStack.length, steps + 1)
s.undo()
assert.equal(serialize(s.layout), layoutBefore, 'one undo restores all port positions')
assert.deepEqual(s.geometry('source').outputs, ['a', 'b'])
// Arrange includes port ordering in the same undo entry as Card placement.
s.arrange([])
assert.equal(s.undoStack.length, steps + 1)
assert.equal(sceneCrossings(), 0)
assert.equal(serialize(s.project), projectBefore)
s.undo()
assert.equal(serialize(s.layout), layoutBefore)

// A nested Workflow has fixed boundary wires as well as ordinary connections.
const outer = await s.makeSubgraph(new Set(['source']), [])
assert.ok(outer)
s.resolution.set(outer + '/source', { ...s.resolution.get('source')!, path: [outer, 'source'] })
const inside = new Scene(s, [outer])
assert.ok(inside.boundary)
// Reverse the inner display order while the outer boundary names stay fixed.
s.updateLayout([outer], l => l.set('cards', parse('{"source":{"x":0,"y":0,"outputOrder":["b","a"]}}')))
assert.equal(sceneCrossings([outer]), 1)
const nestedBefore = serialize(s.layout), nestedSteps = s.undoStack.length
s.untangle([outer])
assert.equal(sceneCrossings([outer]), 0)
assert.equal(s.undoStack.length, nestedSteps + 1)
s.undo()
assert.equal(serialize(s.layout), nestedBefore)

// The end of a drag: a Card moved across its sibling makes two wires cross, and the
// ports around it take the better order in the same undo step as the move.
{
  const d = new ProjectSession('/untangle-drag', 'untangle-drag', 'untangle-drag')
  d.project = parse('{"name":"untangle-drag","canvas":{"cards":[],"connections":[]}}')
  d.layout = parse('{"flow":"topToBottom"}')
  d.scheduleSave = () => {}
  const cards: [string, number, number, string[], string[]][] = [
    ['source', 200, 0, [], ['a', 'b']], ['left', 0, 300, ['in'], []], ['right', 500, 300, ['in'], []],
    // A tangle far away, wired to none of the Cards above.
    ['far', 3200, 0, [], ['a', 'b']], ['farl', 3000, 300, ['in'], []], ['farr', 3500, 300, ['in'], []],
  ]
  for (const [id, x, y, ins, outs] of cards) {
    d.addCard('python', [], { x, y }, id)
    const ports = (names: string[]) => names.map(name => ({ name, kind: D.PortKind.value, optional: false }))
    d.resolution.set(id, { path: [id], inputs: ports(ins), outputs: ports(outs), diagnostics: [] })
  }
  d.connect(['source', 'a'], ['left', 'in'], [])
  d.connect(['source', 'b'], ['right', 'in'], [])
  d.connect(['far', 'a'], ['farr', 'in'], [])
  d.connect(['far', 'b'], ['farl', 'in'], [])
  const crossings = (ids: string[]) => {
    const from = d.connections([]).map(c => str(get(c, 'from', 'card')) ?? '')
    const mine = new Scene(d, []).wires.filter(w => w.connection !== undefined && ids.includes(from[w.connection])).map(w => w.pts)
    return crossingPairs(mine, mine.map(p => ({ a: p[0], b: p[p.length - 1] })))
  }
  assert.deepEqual([crossings(['source']), crossings(['far'])], [0, 1])
  const cv = new CanvasController(d, new WorkflowTab([]))
  const project = serialize(d.project)

  // A move that crosses nothing saves no order.
  let layout = serialize(d.layout), steps = d.undoStack.length
  cv.moveSelection(new Set(['left']), new Set(), new Set(), -40, 0)
  assert.equal(d.hasPortOrder('source') || d.hasPortOrder('left'), false, 'nothing to improve, nothing saved')
  d.undo()
  assert.equal(serialize(d.layout), layout)

  // Moving the left Card to the right of its sibling crosses the two wires.
  cv.moveSelection(new Set(['left']), new Set(), new Set(), 1000, 0)
  assert.deepEqual(d.position('left'), { x: 1000, y: 300 }, 'the Card is where it was dropped')
  assert.deepEqual(d.position('source'), { x: 200, y: 0 }, 'no other Card moves')
  assert.deepEqual(d.geometry('source').outputs, ['b', 'a'], 'the ports follow the Cards')
  assert.equal(crossings(['source']), 0, 'the crossing the move made is gone')
  assert.equal(serialize(d.project), project, 'named connections are unchanged')
  assert.deepEqual(d.geometry('far').outputs, ['a', 'b'], 'ports away from the move stay as they are')
  assert.equal(crossings(['far']), 1)
  assert.equal(d.undoStack.length, steps + 1, 'the move and the port order are one step')
  const moved = serialize(d.layout)
  d.undo()
  assert.equal(serialize(d.layout), layout, 'one undo restores the position and the order')
  d.redo()
  assert.equal(serialize(d.layout), moved)

  // Dropping it again where nothing improves keeps the saved order: the canvas does not flicker.
  cv.moveSelection(new Set(['left']), new Set(), new Set(), 40, 0)
  assert.deepEqual(d.geometry('source').outputs, ['b', 'a'])
  // Moving it back crosses the wires the other way, and the order follows again.
  cv.moveSelection(new Set(['left']), new Set(), new Set(), -1040, 0)
  assert.deepEqual(d.geometry('source').outputs, ['a', 'b'])
  assert.equal(crossings(['source']), 0)

  // Cards that move together keep their wires as they are.
  layout = serialize(d.layout)
  cv.moveSelection(new Set(['far', 'farl', 'farr']), new Set(), new Set(), 100, 0)
  assert.deepEqual(d.geometry('far').outputs, ['a', 'b'], 'a move of the whole group changes no order')

  // "Reset Port Order" puts back the declared order, in one step.
  cv.moveSelection(new Set(['left']), new Set(), new Set(), 1000, 0)
  assert.ok(d.hasPortOrder('source'))
  steps = d.undoStack.length
  d.resetPortOrder(['source', 'right'])
  assert.equal(d.hasPortOrder('source'), false)
  assert.deepEqual(d.geometry('source').outputs, ['a', 'b'], 'the declared order')
  assert.deepEqual(d.position('left'), { x: 1000, y: 300 }, 'resetting moves no Card')
  assert.equal(d.undoStack.length, steps + 1)
  d.resetPortOrder(['source'])
  assert.equal(d.undoStack.length, steps + 1, 'nothing to reset is not an edit')
  d.undo()
  assert.deepEqual(d.geometry('source').outputs, ['b', 'a'])
}
console.log('untangle wires: passed')
