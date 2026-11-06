// The file the Agent is writing (as the Mac app's PresenceTests): it is read out
// of the save's arguments at any cut, played slowly enough to see, goes when the
// call fails, and its panel stays clear of the bird and inside the view.
import assert from 'node:assert/strict'
import { CODE_TIMING as T, fileOfCard, LiveCodePlayer, PANEL, panelFrame, panelScale, panelTail, SourceWritingScanner } from '../src/model/liveCode'

// The arguments, cut anywhere.
const args = JSON.stringify({ base_rev: '1', changes: [
  { value_type: 'PutDefinition', value: { id: 'a', card_json: '{"path":"no"}' } },
  { value_type: 'PutSource', value: { path: 'definitions/a/m.py', bytes: 'é\t"x"\n\u{1F600}' } },
] })
for (const cut of [1, 2, 3, 7, 24]) {
  const s = new SourceWritingScanner()
  for (let i = 0; i < args.length; i += cut) s.feed(args.slice(i, i + cut))
  assert.deepEqual(s.current, { path: 'definitions/a/m.py', text: 'é\t"x"\n\u{1F600}', done: true }, `cut every ${cut}`)
}
const escaped = new SourceWritingScanner()
escaped.feed('{"value":{"path":"definitions/a/b","bytes":"\\u3042\\ud83d\\ude00')
assert.equal(escaped.current?.text, 'あ\u{1F600}')
assert.deepEqual(fileOfCard('definitions/a/src/m.py'), { card: 'a', file: 'src/m.py' })
assert.equal(fileOfCard('layout.json'), undefined)

const file = (card: string, bytes = 'code') => JSON.stringify({ value: { path: `definitions/${card}/m.py`, bytes } }) + ','

// Comes at its pace, not at once, and never past what has arrived.
{
  const p = new LiveCodePlayer()
  p.input(0, '{"changes":[{"value":{"path":"definitions/load/m.py","bytes":"import os', 100)
  assert.equal(p.at(100)?.text, '')
  assert.equal(p.at(100 + 4 / T.rate + 0.001)?.text, 'impo')
  assert.equal(p.at(101)?.text, 'import os')
  assert.equal(p.at(101)?.card, 'load')
  assert.equal(p.at(101)?.file, 'm.py')
  // Arriving all at once does not make it go by in a moment: the next waits, the last lines hold.
  p.input(0, '"}},{"value":{"path":"definitions/rgb/m.py","bytes":"from', 100.05)
  const next = 100 + T.shortest + T.hold
  assert.equal(p.at(next - 0.1)?.card, 'load')
  assert.equal(p.at(next + 1)?.card, 'rgb')
  // A model slower than the pace: the scene lasts as long as the file arrives.
  p.input(0, ' a', next + 0.2)
  p.input(0, ' b', next + 30)
  assert.equal(p.at(next + 30.4)?.text, 'from a b')
  // The save goes through: what still plays says so, and plays on after the call.
  p.saved()
  p.ended(true, next + 30.5)
  assert.equal(p.at(next + 30.6)?.saved, true)
  assert.equal(p.at(next + 40), undefined)
}

// A long file speeds up, and files too far behind are not shown.
{
  const p = new LiveCodePlayer()
  const long = 'x'.repeat(5000)
  p.input(0, '{"changes":[' + file('a', long), 100)
  assert.equal(p.at(100 + T.longest / 2)?.text.length, 2500)
  assert.equal(p.at(100 + T.longest + 0.1)?.text.length, 5000)
  for (let n = 0; n < 20; n++) p.input(0, file(`c${n}`, long), 100)
  const shown = new Set<string>()
  for (let t = 100; t < 200; t += 0.5) { const c = p.at(t)?.card; if (c) shown.add(c) }
  assert.ok(shown.size < 10, `${shown.size} files`)
  assert.equal(p.at(100 + T.backlog + T.longest + T.hold + 1), undefined)
}

// What a failed or stopped call wrote goes at once; what is not a Card's file never shows.
{
  const p = new LiveCodePlayer()
  p.input(0, file('a'), 100)
  p.input(0, file('b'), 100)
  p.ended(false, 100.5)
  assert.equal(p.at(100.5), undefined)
  assert.equal(p.at(104), undefined)
  p.input(1, '{"changes":[{"value":{"path":"layout.json","bytes":"{', 101)
  assert.equal(p.at(101), undefined)
  p.input(2, file('c'), 102)
  assert.equal(p.at(102.5)?.card, 'c')
}

// The lines shown.
assert.deepEqual(panelTail('a\nb', 5), ['a', 'b'])
assert.deepEqual(panelTail('1\n2\n3\n4\n5\n6\n7', 3), ['5', '6', '7'])
assert.deepEqual(panelTail('1\n2\n3\n', 3), ['2', '3', ''])
assert.deepEqual(panelTail('', 3), [''])
assert.deepEqual(panelTail('a\r\nb\n\tc', 3), ['a', 'b', '    c'])

// Where it goes.
{
  const view = { x: 12, y: 52, width: 1000, height: 800 }
  const card = { x: 400, y: 400, width: 220, height: 120 }
  const inside = (f: { x: number; y: number; width: number; height: number }) =>
    f.x >= view.x && f.y >= view.y && f.x + f.width <= view.x + view.width && f.y + f.height <= view.y + view.height
  const above = panelFrame(card, 80, 1, view)!
  assert.ok(above.y + above.height <= card.y - 80)
  assert.equal(above.x + above.width / 2, card.x + card.width / 2)
  assert.deepEqual([above.width, above.height], [PANEL.width, PANEL.height])
  const top = { ...card, y: 100 }
  const under = panelFrame(top, 80, 1, view)!
  assert.ok(under.y >= top.y + top.height && inside(under))
  // Something above it (another row of Cards) and nothing under: under the Card.
  const row = { ...card, y: card.y - 200 }
  assert.ok(panelFrame(card, 80, 1, view, [row])!.y >= card.y + card.height)
  // Something on both sides: above the bird all the same, never over it.
  const both = panelFrame(card, 80, 1, view, [row, { ...card, y: card.y + 200 }])!
  assert.ok(both.y + both.height <= card.y - 80)
  for (const dx of [-500, 500]) assert.ok(inside(panelFrame({ ...card, x: card.x + dx }, 80, 1, view)!))
  assert.ok(inside(panelFrame({ x: 400, y: 100, width: 220, height: 700 }, 80, 1, view)!))
  assert.equal(panelFrame({ ...card, x: 3000 }, 80, 1, view), undefined)
  assert.deepEqual([panelScale(0.5), panelScale(1), panelScale(3)], [0.8, 1, 1.3])
}
console.log('live code behavior: passed')
