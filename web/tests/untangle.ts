import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { crossingPairs, orderedPorts, untanglePorts, UntangleCard } from '../src/model/untangle'
import { WireRouter } from '../src/model/canvasKit'
import { ArrangeWire } from '../src/model/arrange'
import { FlowDirection } from '../src/model/prefs'
import { ProjectSession } from '../src/model/session'
import { parse, serialize } from '../src/shared/json'
import { D } from '../src/shared/rpc'
import { Scene } from '../src/model/canvas'

const card = (id: string, x: number, y: number, inputs: string[], outputs: string[], movable = true): UntangleCard => ({
  id, x, y, width: 240, height: 80, movable,
  inputs: inputs.map((name, i) => ({ name, x: 240 * (i + 1) / (inputs.length + 1), y: 0 })),
  outputs: outputs.map((name, i) => ({ name, x: 240 * (i + 1) / (outputs.length + 1), y: 80 })),
})
const cases = [
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
function count(flow: FlowDirection, cs: UntangleCard[], ws: ArrangeWire[]) {
  const ends = ws.map(w => {
    const a = cs.find(c => c.id === w.from)!, b = cs.find(c => c.id === w.to)!
    const p = a.outputs.find(p => p.name === w.fromPort)!, q = b.inputs.find(p => p.name === w.toPort)!
    return { a: { x: a.x + p.x, y: a.y + p.y }, b: { x: b.x + q.x, y: b.y + q.y } }
  })
  return crossingPairs(new WireRouter(flow, cs).routeAll(ends).map(r => r.pts), ends)
}
const fixtures: unknown[] = []
for (const c of cases) for (const flow of ['topToBottom', 'leftToRight'] as const) {
  const cards = flow === 'topToBottom' ? c.cards : c.cards.map(c => ({ ...c, x: c.y, y: c.x, width: c.height, height: c.width,
    inputs: c.inputs.map(p => ({ name: p.name, x: p.y, y: p.x })), outputs: c.outputs.map(p => ({ name: p.name, x: p.y, y: p.x })) }))
  const before = JSON.stringify(cards)
  const orders = untanglePorts(flow, cards, c.wires)
  assert.equal(JSON.stringify(cards), before, 'input is not mutated')
  const after = cards.map(c => {
    const o = orders.find(o => o.id === c.id)
    return !o ? c : { ...c, inputs: c.inputs.map((p, i) => ({ ...p, name: o.inputs[i] })), outputs: c.outputs.map((p, i) => ({ ...p, name: o.outputs[i] })) }
  })
  assert.equal(count(flow, cards, c.wires), c.cards.length > 3 ? 2 : 1, c.name + ': starts crossed')
  assert.equal(count(flow, after, c.wires), c.name.includes('fixed ports remaining') || c.name === 'fixed routing ports' ? 1 : 0, c.name + ': crossings removed when slots may move')
  if (c.name === 'multiple Cards and connections') assert.equal(after[0].outputs[1].name, 'unused', 'unconnected ports retain their slots')
  assert.deepEqual(untanglePorts(flow, after, c.wires), [], 'running twice does not move more ports')
  fixtures.push({ name: c.name, flow, cards, wires: c.wires, expect: orders })
}
const fixturePath = path.join(process.argv[2], 'tests/fixtures/untangle-contract.json')
if (process.argv.includes('--update-untangle')) fs.writeFileSync(fixturePath, '[\n' + fixtures.map(c => JSON.stringify(c)).join(',\n') + '\n]\n')
assert.deepEqual(JSON.parse(fs.readFileSync(fixturePath, 'utf8')), fixtures, 'Mac and Web share the same cases')
assert.deepEqual(orderedPorts([{ name: 'a' }, { name: 'b' }, { name: 'new' }], ['gone', 'b', 'b', 'a']).map(p => p.name), ['b', 'a', 'new'])

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
console.log('untangle wires: passed')
