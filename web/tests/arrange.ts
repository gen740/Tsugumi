// The Arrange button. tests/fixtures/arrange-contract.json has what Web and Swift
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
const { CardGeometry, snapToGrid, WireRouter } = await import('../src/model/canvasKit')
const { crossed } = await import('./wireDemos')
const { CanvasController } = await import('../src/model/canvas')
const { ProjectSession } = await import('../src/model/session')
const { WorkflowTab } = await import('../src/model/workspace')
const { parse, get, dbl } = await import('../src/shared/json')

const root = process.argv[2]
const fixture = (name: string) => path.join(root, 'tests/fixtures', name)
const read = (name: string) => JSON.parse(fs.readFileSync(fixture(name), 'utf8'))

interface NodeCard { id: string; kind: string; summary: boolean; params: number; inputs: string[]; outputs: string[]; display: boolean; collapsed?: boolean }
interface NodeCase { name: string; flow: FlowDirection; cards: NodeCard[]; wires: ArrangeInput['wires']; frames: ArrangeInput['frames']; notes: ArrangeInput['notes']; boundary?: ArrangeInput['boundary'] }

/** What the canvas gives `arrangeCards` for this graph, from `CardGeometry`. */
function inputOf(c: NodeCase, flow: FlowDirection, grid: number | undefined): ArrangeInput {
  return {
    flow, grid: grid ?? 20, snap: grid !== undefined, wires: c.wires, frames: c.frames, notes: c.notes, boundary: c.boundary ?? { inputs: [], outputs: [] },
    cards: c.cards.map((n) => {
      const g = new CardGeometry({
        title: n.id, kind: n.kind, summary: n.summary ? 's' : undefined, inputs: n.inputs.map((name) => ({ name })), outputs: n.outputs.map((name) => ({ name })),
        params: Array.from({ length: n.params }, (_, i) => ({ name: 'p' + i, type: 'int64', value: '0' })), invocation: 0n, runContinues: false, errors: [], warnings: [],
        collapsed: n.collapsed ?? false, hasDisplay: n.display, portGrid: grid,
      } as ConstructorParameters<typeof CardGeometry>[0], flow)
      const ports = (names: string[], output: boolean) => names.map((name) => ({ name, ...g.port(name, output) }))
      return { id: n.id, ...g.size, inputs: ports(n.inputs, false), outputs: ports(n.outputs, true) }
    }),
  }
}

const opposite = (f: FlowDirection): FlowDirection => (f === 'topToBottom' ? 'leftToRight' : 'topToBottom')
const cases: NodeCase[] = [...read('arrange-demos.json'), ...read('arrange-cases.json')]
/** Each graph in its flow with the grid on and off, and in the other flow with the grid on. */
const inputs = cases.flatMap((c) => [
  { name: `${c.name} ${c.flow} grid20`, input: inputOf(c, c.flow, 20) },
  { name: `${c.name} ${c.flow} off`, input: inputOf(c, c.flow, undefined) },
  { name: `${c.name} ${opposite(c.flow)} grid20`, input: inputOf(c, opposite(c.flow), 20) },
])

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

function noOverlap(input: ArrangeInput, r: ArrangeResult) {
  const boxes = [...box(input, r), ...bars(input, r)]
  return boxes.every((a, i) => boxes.slice(i + 1).every((b) => !(a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height)))
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
  checkPlaced(e.name, e.input, r)
})

/** What the canvas does with a layout: nothing is moved by the grid, and no wire crosses a Card. */
function checkPlaced(name: string, input: ArrangeInput, r: ArrangeResult) {
  const g = input.grid
  for (const f of [...r.frames, ...r.notes]) assert.ok(onGrid(f.x, g) && onGrid(f.y, g), `${name}: ${f.id} on the grid`)
  const vertical = input.flow === 'topToBottom'
  for (const c of r.cards) {
    assert.ok(onGrid(vertical ? c.y : c.x, g), `${name}: ${c.id} along the flow`)
    if (input.snap) {
      assert.equal(snapToGrid(c.x, g), c.x, `${name}: the grid leaves ${c.id} where it is`)
      assert.equal(snapToGrid(c.y, g), c.y, `${name}: the grid leaves ${c.id} where it is`)
    }
  }
  const cards = box(input, { ...r, frames: [], notes: [] })
  const framesAt = box(input, { ...r, cards: [], notes: [] })
  const barRects = bars(input, r)
  const router = new WireRouter(input.flow, [...cards, ...framesAt, ...barRects])
  const port = (a: { x: number; y: number }, p: { x: number; y: number }) => ({ x: a.x + p.x, y: a.y + p.y })
  const at = (id: string) => r.cards.find((c) => c.id === id)!
  const wires: { a: { x: number; y: number }; b: { x: number; y: number }; name: string }[] = []
  for (const w of input.wires) {
    const from = input.cards.find((c) => c.id === w.from), to = input.cards.find((c) => c.id === w.to)
    const fp = from?.outputs.find((p) => p.name === w.fromPort), tp = to?.inputs.find((p) => p.name === w.toPort)
    if (fp && tp) wires.push({ a: port(at(w.from), fp), b: port(at(w.to), tp), name: `${w.from}.${w.fromPort}>${w.to}.${w.toPort}` })
  }
  const side = (list: { card: string; port: string }[], bar: { x: number; y: number; width: number; height: number } | undefined, output: boolean) => list.forEach((e, i) => {
    const c = input.cards.find((x) => x.id === e.card)
    const p = c && (output ? c.outputs : c.inputs).find((q) => q.name === e.port)
    if (!bar || !p) return
    const end = { x: bar.x + (bar.width * (i + 1)) / (list.length + 1), y: output ? bar.y : bar.y + bar.height }
    wires.push(output ? { a: port(at(e.card), p), b: end, name: `${e.card}.${e.port}>out` } : { a: end, b: port(at(e.card), p), name: `in>${e.card}.${e.port}` })
  })
  side(input.boundary?.inputs ?? [], barRects[0], false)
  side(input.boundary?.outputs ?? [], barRects[1], true)
  const routes = router.routeAll(wires)
  wires.forEach((w, i) => assert.equal(crossed(routes[i].pts, [...cards, ...framesAt, ...barRects]), 0, `${name}: ${w.name} crosses a Card or a frame`))
}

// Another spacing of the grid: the same holds.
for (const c of cases) {
  for (const flow of [c.flow, opposite(c.flow)]) {
    const input = inputOf(c, flow, 30)
    const r = arrangeCards(input)
    checkPlaced(`${c.name} ${flow} grid30`, input, r)
    assert.ok(noOverlap(input, r), `${c.name} ${flow} grid30: overlap`)
  }
}

// ---- The two layouts asked for -------------------------------------------------------
// Target positions (Card's top-left, relative to settings); the x of a whole
// stack may differ as long as the wires are straight, so what is held here is
// how far each Card is from them after the best shift across the flow.
const targets: Record<string, Record<string, [number, number]>> = {
  report: { start: [40, -80], settings: [0, 0], share: [165, 180], generate: [-60, 260], aggregate: [-60, 400], render: [60, 520], archive: [60, 660] },
  'live-stream': { start: [60, -80], settings: [0, 0], loop: [80, 280], generate: [-20, 360], plot: [-20, 500] },
}
for (const [name, target] of Object.entries(targets)) {
  const c = expected.find((e) => e.name === `${name} topToBottom grid20`)!
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
  const c = expected.find((e) => e.name === 'report topToBottom grid20')!
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
  const frame = s.displayFrame(b)
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

  // Moving one Card afterwards: it goes by whole grids and nothing else moves;
  // moving everything keeps the arrangement.
  const cv = new CanvasController(s, new WorkflowTab([]))
  const others = [a, c].map((k) => s.position(k))
  const was = s.position(b), frameWas = serialize(s.displayFrame(b)!)
  cv.moveSelection(new Set([b]), new Set(), new Set(), 47, 33, 20)
  assert.deepEqual(s.position(b), { x: was.x + 40, y: was.y + 40 })
  assert.deepEqual([a, c].map((k) => s.position(k)), others, 'the other Cards stay')
  assert.equal(serialize(s.displayFrame(b)!), frameWas, 'a placed frame stays')
  s.undo()
  const noteWas = { x: dbl(get(s.note(note), 'x'))!, y: dbl(get(s.note(note), 'y'))! }
  const frameAt = { x: dbl(get(s.displayFrame(b), 'x'))!, y: dbl(get(s.displayFrame(b), 'y'))! }
  cv.moveSelection(new Set([a, b, c]), new Set([note]), new Set([b]), 47, 33, 20)
  assert.deepEqual([a, c].map((k) => s.position(k)), others.map((p) => ({ x: p.x + 40, y: p.y + 40 })))
  assert.deepEqual({ x: dbl(get(s.note(note), 'x')), y: dbl(get(s.note(note), 'y')) }, { x: noteWas.x + 40, y: noteWas.y + 40 })
  assert.deepEqual({ x: dbl(get(s.displayFrame(b), 'x')), y: dbl(get(s.displayFrame(b), 'y')) }, { x: frameAt.x + 40, y: frameAt.y + 40 })
}

console.log('arrange behavior: passed')
