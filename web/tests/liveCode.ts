// The file the Agent is writing (as the Mac app's PresenceTests): it is read out
// of the save's arguments at any cut, shown as far as it has arrived, goes when
// the save is through or the call fails, and its panel stays clear of the bird
// and inside the view.
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

// What has arrived is written out up to the next lump, never past what has arrived.
{
  const p = new LiveCodePlayer()
  p.input(0, '{"changes":[{"value":{"path":"definitions/load/m.py","bytes":"import os', 100)
  assert.equal(p.at(100)?.text, '')
  assert.equal(p.at(100 + T.ease / 2)?.text, 'impo')
  assert.equal(p.at(100 + T.ease + 0.01)?.text, 'import os')
  assert.equal(p.at(100)?.card, 'load')
  // The next lump, before this one has been written out: on from where it has got to, never back.
  p.input(0, ' 1', 100 + T.ease / 2)
  assert.equal(p.at(100 + T.ease / 2)?.text, 'impo')
  assert.equal(p.at(100 + T.ease * 1.5 + 0.01)?.text, 'import os 1')
  assert.equal(p.at(100)?.file, 'm.py')
  // A slow model: the scene lasts as long as the file arrives, a pause included.
  p.input(0, ' a', 130)
  assert.equal(p.at(130 + T.stalled - 0.1)?.text, 'import os 1 a')
  // The next file comes: the one on the screen has been seen long enough, so it
  // does not hold, and the next shows at once.
  p.input(0, '"}},{"value":{"path":"definitions/rgb/m.py","bytes":"from', 131)
  assert.equal(p.at(131)?.card, 'rgb')
  assert.equal(p.at(131 + T.ease + 0.01)?.text, 'from')
  // The last file has arrived and the save goes through: it says so for its hold,
  // and is gone by then.
  p.input(0, ' a"}}]}', 133)
  p.ended(true, 133.1)
  p.saved(133.1)
  assert.equal(p.at(133.2)?.saved, true)
  assert.equal(p.at(133 + T.hold + 0.01), undefined)
}

// A short file stays long enough to see, and the window does not fall behind.
{
  const p = new LiveCodePlayer()
  p.input(0, file('a'), 100)
  assert.equal(p.at(100 + T.shortest + T.hold - 0.01)?.card, 'a')
  assert.equal(p.at(100 + T.shortest + T.hold + 0.01), undefined)
  // More come at once: the first keeps its shortest stay, then only the newest shows.
  p.input(0, file('b'), 100.1)
  p.input(0, file('c'), 100.2)
  assert.equal(p.at(100 + T.shortest - 0.01)?.card, 'a')
  assert.equal(p.at(100 + T.shortest + 0.01)?.card, 'c')
}

// A save ends what waits, and a run ends the window.
{
  const p = new LiveCodePlayer()
  p.input(0, file('a'), 100)
  p.input(0, file('b'), 100.1)
  p.saved(100.2)
  p.ended(true, 100.2)
  assert.equal(p.at(100.3)?.saved, true)
  assert.equal(p.at(100 + T.shortest + 0.01), undefined)
  p.input(1, file('c'), 110)
  p.saved(110.1)
  p.ended(true, 110.1)
  assert.equal(p.at(110.2)?.card, 'c')
  p.stop()
  assert.equal(p.at(110.2), undefined)
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
