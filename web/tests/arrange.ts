// The 整列 button. tests/fixtures/arrange-contract.json has what Web and Swift
// must both give: this file checks it here, macOS/Tests/.../ArrangeTests.swift
// there. After changing CardGeometry or the layout on purpose:
//   cd web && npm test -- --update-arrange
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { arrangeCards, type ArrangeInput, type ArrangeResult } from '../src/model/arrange'
import { serialize } from '../src/shared/json'
import type { FlowDirection } from '../src/model/prefs'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { CardGeometry } = await import('../src/model/canvasKit')
const { key, ProjectSession } = await import('../src/model/session')
const { parse, get, dbl } = await import('../src/shared/json')

const root = process.argv[2]
const fixture = (name: string) => path.join(root, 'tests/fixtures', name)
const read = (name: string) => JSON.parse(fs.readFileSync(fixture(name), 'utf8'))

interface NodeCard { id: string; kind: string; summary: boolean; params: number; inputs: string[]; outputs: string[]; display: boolean; collapsed?: boolean }
interface NodeCase { name: string; flow: FlowDirection; cards: NodeCard[]; wires: ArrangeInput['wires']; frames: ArrangeInput['frames']; notes: ArrangeInput['notes']; boundary?: ArrangeInput['boundary'] }

/** What the canvas gives `arrangeCards` for this graph, from `CardGeometry`. */
function inputOf(c: NodeCase, flow: FlowDirection): ArrangeInput {
  return {
    flow, grid: 20, wires: c.wires, frames: c.frames, notes: c.notes, boundary: c.boundary ?? { inputs: [], outputs: [] },
    cards: c.cards.map((n) => {
      const g = new CardGeometry({
        title: n.id, kind: n.kind, summary: n.summary ? 's' : undefined, inputs: n.inputs.map((name) => ({ name })), outputs: n.outputs.map((name) => ({ name })),
        params: Array.from({ length: n.params }, (_, i) => ({ name: 'p' + i, type: 'int64', value: '0' })), invocation: 0n, runContinues: false, errors: [], warnings: [],
        collapsed: n.collapsed ?? false, hasDisplay: n.display,
      } as ConstructorParameters<typeof CardGeometry>[0], flow)
      const ports = (names: string[], output: boolean) => names.map((name) => ({ name, ...g.port(name, output) }))
      return { id: n.id, ...g.size, inputs: ports(n.inputs, false), outputs: ports(n.outputs, true) }
    }),
  }
}

const opposite = (f: FlowDirection): FlowDirection => (f === 'topToBottom' ? 'leftToRight' : 'topToBottom')
const cases: NodeCase[] = [...read('arrange-demos.json'), ...read('arrange-cases.json')]
const inputs = cases.flatMap((c) => [c.flow, opposite(c.flow)].map((flow) => ({ name: `${c.name} ${flow}`, input: inputOf(c, flow) })))

const contract = 'arrange-contract.json'
if (process.argv.includes('--update-arrange')) {
  const lines = inputs.map((i) => JSON.stringify({ name: i.name, input: i.input, expect: arrangeCards(i.input) }))
  fs.writeFileSync(fixture(contract), '[\n' + lines.join(',\n') + '\n]\n')
}

const box = (input: ArrangeInput, r: ArrangeResult) => [
  ...r.cards.map((a) => { const c = input.cards.find((x) => x.id === a.id)!; return { id: 'card ' + a.id, x: a.x, y: a.y, width: c.width, height: c.height } }),
  ...r.frames.map((a) => { const f = input.frames.find((x) => x.id === a.id)!; return { id: 'frame ' + a.id, x: a.x, y: a.y, width: f.width, height: f.height } }),
  ...r.notes.map((a) => { const n = input.notes.find((x) => x.id === a.id)!; return { id: 'note ' + a.id, x: a.x, y: a.y, width: n.width, height: n.height } }),
]

/** The bars a Card in Card draws around its Cards when they have not been moved. */
function bars(input: ArrangeInput, r: ArrangeResult) {
  const ins = input.boundary?.inputs.length ?? 0, outs = input.boundary?.outputs.length ?? 0
  if ((!ins && !outs) || !r.cards.length) return []
  const cards = box(input, { ...r, frames: [], notes: [] })
  const left = Math.min(...cards.map((c) => c.x)), right = Math.max(...cards.map((c) => c.x + c.width))
  const top = Math.min(...cards.map((c) => c.y)), bottom = Math.max(...cards.map((c) => c.y + c.height))
  const width = Math.max(right - left, Math.max(ins, outs) * 110 + 40), x = (left + right) / 2 - width / 2
  return [{ id: 'bar in', x, y: top - 96, width, height: 40 }, { id: 'bar out', x, y: bottom + 56, width, height: 40 }]
}

const onGrid = (v: number, g: number) => Math.abs(v / g - Math.round(v / g)) < 1e-6

const expected: { name: string; input: ArrangeInput; expect: ArrangeResult }[] = read(contract)
assert.deepEqual(expected.map((e) => e.name), inputs.map((i) => i.name), 'the contract has every case (npm test -- --update-arrange)')
expected.forEach((e, n) => {
  assert.deepEqual(inputs[n].input, e.input, `${e.name}: CardGeometry gives what the contract says (npm test -- --update-arrange after changing it)`)
  const r = arrangeCards(e.input)
  assert.deepEqual(r, e.expect, e.name)
  assert.deepEqual(arrangeCards(JSON.parse(JSON.stringify(e.input))), r, `${e.name}: the same input, the same result`)
  assert.equal(r.cards.length, e.input.cards.length, `${e.name}: every Card has a place`)
  assert.equal(r.frames.length, e.input.frames.length, `${e.name}: every frame has a place`)
  assert.equal(r.notes.length, e.input.notes.length, `${e.name}: every note has a place`)

  const boxes = [...box(e.input, r), ...bars(e.input, r)]
  for (let i = 0; i < boxes.length; i++) {
    for (let j = i + 1; j < boxes.length; j++) {
      const a = boxes[i], b = boxes[j]
      assert.ok(!(a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height), `${e.name}: ${a.id} and ${b.id} overlap`)
    }
  }
  // Along the flow everything is on the grid; across it, a Card leaves it only
  // to keep a wire straight. Frames and notes are always on it.
  for (const c of r.cards) assert.ok(onGrid(e.input.flow === 'topToBottom' ? c.y : c.x, 20), `${e.name}: ${c.id} along the flow`)
  for (const f of [...r.frames, ...r.notes]) assert.ok(onGrid(f.x, 20) && onGrid(f.y, 20), `${e.name}: ${f.id} on the grid`)
})

// ---- The two layouts asked for -------------------------------------------------------
// Target positions (Card's top-left, relative to settings); the x of a whole
// stack may differ as long as the wires are straight, so what is held here is
// how far each Card is from them after the best shift across the flow.
const targets: Record<string, Record<string, [number, number]>> = {
  report: { start: [40, -80], settings: [0, 0], share: [165, 180], generate: [-60, 260], aggregate: [-60, 400], render: [60, 520], archive: [60, 660] },
  'live-stream': { start: [60, -80], settings: [0, 0], loop: [80, 280], generate: [-20, 360], plot: [-20, 500] },
}
for (const [name, target] of Object.entries(targets)) {
  const c = expected.find((e) => e.name === `${name} topToBottom`)!
  const r = arrangeCards(c.input)
  const at = new Map(r.cards.map((a) => [a.id, a]))
  const settings = at.get('settings')!
  for (const [id, [, y]] of Object.entries(target)) assert.ok(Math.abs(at.get(id)!.y - settings.y - y) <= 60, `${name}: ${id} is level with where it was asked for`)
  // The order across the flow is the one asked for, wherever it could be told apart.
  const across = Object.entries(target).map(([id, [x]]) => ({ id, want: x, got: at.get(id)!.x - settings.x }))
  const shift = across.map((a) => a.got - a.want).sort((a, b) => a - b)[Math.floor(across.length / 2)]
  for (const a of across) assert.ok(Math.abs(a.got - a.want - shift) <= 100, `${name}: ${a.id} across the flow (${a.got} for ${a.want}, shifted by ${shift})`)
}

// The wires of `report`: what could be straight is.
{
  const c = expected.find((e) => e.name === 'report topToBottom')!
  const r = arrangeCards(c.input)
  const straight = c.input.wires.filter((w) => {
    const from = c.input.cards.find((x) => x.id === w.from)!, to = c.input.cards.find((x) => x.id === w.to)!
    const f = r.cards.find((x) => x.id === w.from)!, t = r.cards.find((x) => x.id === w.to)!
    return Math.abs(f.x + from.outputs.find((p) => p.name === w.fromPort)!.x - t.x - to.inputs.find((p) => p.name === w.toPort)!.x) < 0.5
  }).map((w) => w.to)
  assert.deepEqual(straight.sort(), ['aggregate', 'archive', 'render', 'settings', 'share'])
}

// ---- The button --------------------------------------------------------------------
{
  const s = new ProjectSession('/p', 'k', 'p')
  s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  s.layout = parse('{"flow":"topToBottom"}')
  s.scheduleSave = () => {} // no tsugumid here
  const a = s.addCard('trigger', [], { x: 500, y: 500 }), b = s.addCard('python', [], { x: 0, y: 0 }), c = s.addCard('logger', [], { x: 7, y: 9 })
  const note = s.addNote([], { x: 3, y: 4 })
  s.updateDisplayFrame(b, (d) => { d.set('width', parse('300')); d.set('height', parse('200')) })
  s.setCollapsed([c], true)
  const before = serialize(s.layout)
  const steps = s.undoStack.length
  s.arrange([])
  assert.equal(s.undoStack.length, steps + 1, 'arranging is one step of undo')
  assert.notEqual(serialize(s.layout), before)
  const frame = s.displayFrame(key([b]))
  assert.equal(dbl(get(frame, 'width')), 300, 'the frame keeps its size')
  assert.notEqual(dbl(get(frame, 'x')), undefined, 'and has a place')
  assert.notDeepEqual(s.position(a), { x: 500, y: 500 })
  assert.ok(dbl(get(s.note(note), 'x')) !== 3 || dbl(get(s.note(note), 'y')) !== 4, 'the note moved')
  const after = serialize(s.layout)
  s.undo()
  assert.equal(serialize(s.layout), before, 'one undo puts everything back')
  s.arrange([])
  assert.equal(serialize(s.layout), after, 'from the same graph, the same layout')
  s.arrange([])
  assert.equal(serialize(s.layout), after, 'arranging again changes nothing')
}

console.log('arrange behavior: passed')
