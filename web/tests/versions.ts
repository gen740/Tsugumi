// A version's hover: the summary is written from its message and what it
// changed, and the minimap fits its Cards and draws each with its canvas shape.
import assert from 'node:assert/strict'

const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { miniMapLayout } = await import('../src/ui/versions')
const { cardKindNamed, versionSummaryInput } = await import('../src/model/session')
const { CardGeometry } = await import('../src/model/canvasKit')
type CardKind = import('../src/model/session').CardKind
type CardGeometry = import('../src/model/canvasKit').CardGeometry
type CardNodeData = import('../src/model/canvasKit').CardNodeData

/** A Card with the ports a minimap gives it. */
function nodeData(kind: CardKind, fan: number): CardNodeData {
  const ports = (n: number) => Array.from({ length: n }, (_, i) => ({ name: `p${i}` }))
  return {
    title: 'c', kind, inputs: ports(kind === 'merge' ? fan : 1), outputs: ports(kind === 'fork' ? fan : 1), params: [],
    invocation: 0n, runContinues: false, errors: [], warnings: [], collapsed: false, hasDisplay: false,
  }
}

assert.equal(
  versionSummaryInput({ message: '実行: bands\n', changed: ['bands', ''] }, { text: '+x\n', truncated: true }),
  'Commit message: 実行: bands\nChanged Cards: bands, the Workflow itself\n\nDiff (cut short):\n+x\n',
)

const card = (id: string, x: number, y: number, kind: CardKind = 'code', fan = 0, collapsed = false) => ({ id, x, y, kind, fan, collapsed })
const overlap = (a: { x: number; y: number; w: number; h: number }, b: typeof a) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h

// Every Card stays inside the box, in the order it has on the canvas.
const wide = miniMapLayout([card('a', 100, 0), card('b', 5000, 300), card('c', 2500, -400)], 'leftToRight', 284, 132)
for (const c of wide.cards) {
  assert.ok(c.x >= 0 && c.x + c.w <= 284, `${c.id} is inside across`)
  assert.ok(c.y >= 0 && c.y + c.h <= wide.height && wide.height <= 132, `${c.id} is inside down`)
}
assert.ok(wide.cards[0].x < wide.cards[2].x && wide.cards[2].x < wide.cards[1].x)
assert.equal(wide.named, false, 'too small for names')
// A small graph is not blown up, and is centered with its names.
const small = miniMapLayout([card('a', 0, 0)], 'leftToRight', 284, 132)
assert.equal(small.cards[0].w, 220 * 0.3)
assert.equal(small.cards[0].x, (284 - small.cards[0].w) / 2)
assert.equal(small.named, true)
assert.equal(small.height, small.cards[0].h + 16, 'the box is only as tall as the graph')
assert.deepEqual(miniMapLayout([], 'leftToRight', 284, 132).cards, [])

// What is not a Card has the outline and size it has on the canvas, so a graph
// laid out on the canvas does not overlap here: start -> fork -> two Cards.
const flow = 'leftToRight'
const fork = new CardGeometry(nodeData('fork', 2), flow), start = new CardGeometry(nodeData('trigger', 0), flow)
const forked = miniMapLayout([
  card('start', 0, 30, 'trigger'), card('fork', start.size.width + 10, 20, 'fork', 2),
  card('load', start.size.width + fork.size.width + 40, -30), card('draft', start.size.width + fork.size.width + 40, 60, 'draft'),
], flow, 284, 132)
const at = Object.fromEntries(forked.cards.map((c) => [c.id, c]))
assert.equal(at.start.shape, 'start')
assert.equal(at.fork.shape, 'fork')
assert.equal(at.load.shape, 'card')
assert.equal(at.draft.shape, 'card')
assert.equal(at.draft.draft, true, 'a Draft has its dashed outline')
assert.equal(at.load.draft, false)
for (const [i, a] of forked.cards.entries()) for (const b of forked.cards.slice(i + 1)) assert.ok(!overlap(a, b), `${a.id} and ${b.id} do not overlap`)
// The shapes are those of the canvas, scaled and at their place.
const k = at.fork.w / fork.size.width
assert.ok(k > 0 && k <= 0.3)
assert.equal(at.fork.h, fork.size.height * k)
assert.equal(at.start.w, start.size.width * k)
const outlineOf = (g: CardGeometry, c: { x: number; y: number }, scale: number) => {
  const o = g.outlineRect
  return g.outline({ x: c.x + o.x * scale, y: c.y + o.y * scale, width: o.width * scale, height: o.height * scale }, scale)
}
assert.equal(at.fork.outline, outlineOf(fork, at.fork, k))
assert.equal(at.start.outline, outlineOf(start, at.start, k))
// A triangle with its point upstream: it starts at the middle of the left side.
const leftmost = (d: string) => Math.min(...[...d.matchAll(/(-?[\d.]+),(-?[\d.]+)/g)].map((m) => Number(m[1])))
assert.ok(Math.abs(leftmost(at.fork.outline) - (at.fork.x + fork.outlineRect.x * k)) < 1e-6, 'the outline leaves the room the canvas leaves for its wires')

// A branch is a diamond whose corners shrink with it; a merge is a triangle that
// grows with its inputs, and a fork with its outputs; both follow the flow.
const diamond = miniMapLayout([card('b', 0, 0, 'branch')], flow, 284, 132).cards[0]
const canvasDiamond = new CardGeometry(nodeData('branch', 0), flow)
assert.equal(diamond.shape, 'branch')
assert.equal(diamond.w, canvasDiamond.size.width * 0.3)
assert.equal(diamond.outline, outlineOf(canvasDiamond, diamond, 0.3))
assert.notEqual(diamond.outline, canvasDiamond.outline({ x: diamond.x + canvasDiamond.outlineRect.x * 0.3, y: diamond.y, width: canvasDiamond.outlineRect.width * 0.3, height: canvasDiamond.outlineRect.height * 0.3 }))
const merge = (n: number, f: 'leftToRight' | 'topToBottom') => miniMapLayout([card('m', 0, 0, 'merge', n)], f, 284, 132).cards[0]
assert.equal(merge(2, flow).shape, 'merge')
assert.ok(merge(6, flow).h > merge(2, flow).h, 'more inputs, a wider edge')
assert.ok(merge(6, 'topToBottom').w > merge(2, 'topToBottom').w)
assert.notEqual(merge(2, flow).outline, merge(2, 'topToBottom').outline)
const fan = (n: number) => miniMapLayout([card('f', 0, 0, 'fork', n)], flow, 284, 132).cards[0]
assert.ok(fan(6).h > fan(2).h, 'more outputs, a wider edge')

// A loop is a merge and a branch with a wire back: wires are the client's, so
// the two only need not overlap and keep their shapes.
const loop = miniMapLayout([card('m', 0, 0, 'merge', 2), card('step', 140, 0), card('b', 400, 0, 'branch')], flow, 284, 132)
assert.deepEqual(loop.cards.map((c) => c.shape), ['merge', 'card', 'branch'])
for (const [i, a] of loop.cards.entries()) for (const b of loop.cards.slice(i + 1)) assert.ok(!overlap(a, b))

// Only a Card or the start has a name inside it; a folded Card is its header.
const folded = miniMapLayout([card('a', 0, 0, 'code', 0, true)], flow, 284, 132).cards[0]
assert.equal(folded.h, CardGeometry.header * 0.3)
// The kinds as the daemon names them.
assert.equal(cardKindNamed('fork'), 'fork')
assert.equal(cardKindNamed('something new'), 'code')
