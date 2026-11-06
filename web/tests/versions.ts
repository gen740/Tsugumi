// A version's hover: the summary is written from its message and what it
// changed, and the minimap fits its Cards.
import assert from 'node:assert/strict'

const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { miniMapLayout } = await import('../src/ui/versions')
const { versionSummaryInput } = await import('../src/model/session')

assert.equal(
  versionSummaryInput({ message: '実行: bands\n', changed: ['bands', ''] }, { text: '+x\n', truncated: true }),
  'Commit message: 実行: bands\nChanged Cards: bands, the Workflow itself\n\nDiff (cut short):\n+x\n',
)

// Every Card stays inside the box, in the order it has on the canvas.
const wide = miniMapLayout([{ id: 'a', x: 100, y: 0 }, { id: 'b', x: 5000, y: 300 }, { id: 'c', x: 2500, y: -400 }], 284, 132)
for (const c of wide.cards) {
  assert.ok(c.x >= 0 && c.x + wide.w <= 284, `${c.id} is inside across`)
  assert.ok(c.y >= 0 && c.y + wide.h <= wide.height && wide.height <= 132, `${c.id} is inside down`)
}
assert.ok(wide.cards[0].x < wide.cards[2].x && wide.cards[2].x < wide.cards[1].x)
assert.equal(wide.named, false, 'too small for names')
// A small graph is not blown up, and is centered with its names.
const small = miniMapLayout([{ id: 'a', x: 0, y: 0 }], 284, 132)
assert.equal(small.w, 220 * 0.3)
assert.equal(small.cards[0].x, (284 - small.w) / 2)
assert.equal(small.named, true)
assert.equal(small.height, 80 * 0.3 + 16, 'the box is only as tall as the graph')
assert.deepEqual(miniMapLayout([], 284, 132).cards, [])
