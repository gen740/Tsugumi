// Actual streamed source text, including patches, is shown without synthetic typing.
import assert from 'node:assert/strict'
import { CODE_TIMING as T, fileOfCard, LiveCodePlayer, PANEL, panelFrame, panelFreeFrame, panelScale, panelTail, SourceWritingScanner } from '../src/model/liveCode'

const args = JSON.stringify({ changes: [{ value_type: 'PutSource', value: { path: 'definitions/a/m.py', bytes: 'é\t"x"\n😀' } }] })
for (let cut = 1; cut < args.length; cut++) {
  const scanner = new SourceWritingScanner()
  scanner.feed(args.slice(0, cut)); scanner.feed(args.slice(cut))
  assert.equal(scanner.current?.path, 'definitions/a/m.py')
  assert.equal(scanner.current?.text, 'é\t"x"\n😀')
  assert.equal(scanner.current?.done, true)
  assert.equal(scanner.current?.ready, true)
}
const patchArgs = JSON.stringify({ changes: [{ value_type: 'PatchSource', value: {
  patches: [{ offset: 4, remove: 1, text: 'first' }, { text: '置換 😀', offset: 9, remove: 2 }],
  path: 'definitions/a/main.py', base_hash: 'hash',
} }] })
for (let cut = 1; cut < patchArgs.length; cut++) {
  const scanner = new SourceWritingScanner()
  scanner.feed(patchArgs.slice(0, cut)); scanner.feed(patchArgs.slice(cut))
  assert.equal(scanner.current?.path, 'definitions/a/main.py', 'late path reaches patch text')
  assert.equal(scanner.current?.text, '置換 😀', 'last replacement is not passed off as a whole file')
  assert.equal(scanner.current?.patch, true)
  assert.equal(scanner.current?.done, true)
}
assert.deepEqual(fileOfCard('definitions/a/src/m.py'), { card: 'a', file: 'src/m.py' })
assert.equal(fileOfCard('layout.json'), undefined)
const file = (card: string, bytes = 'code') => JSON.stringify({ value: { path: `definitions/${card}/m.py`, bytes } }) + ','

{
  const p = new LiveCodePlayer()
  p.input(0, '{"changes":[{"value":{"path":"definitions/load/m.py","bytes":"import os', 100)
  assert.equal(p.at(100)?.text, 'import os')
  assert.equal(p.at(100.2)?.text, 'import os', 'a clock tick does not type additional characters')
  p.input(0, ' 1', 100.3)
  assert.equal(p.at(100.3)?.text, 'import os 1')
  p.input(0, ' a', 130)
  assert.equal(p.at(130 + T.stalled - 0.1)?.text, 'import os 1 a')
  p.input(0, '"}},{"value":{"path":"definitions/rgb/m.py","bytes":"from', 131)
  assert.equal(p.at(131)?.card, 'rgb')
  assert.equal(p.at(131)?.text, 'from')
  p.input(0, ' a"}}]}', 133)
  p.ended(true, 133.1); p.saved(133.1)
  assert.equal(p.at(133.2)?.saved, true)
  assert.equal(p.at(133 + T.hold + 0.01), undefined)
}
{
  const p = new LiveCodePlayer()
  p.input(0, file('a'), 100)
  p.input(0, file('b'), 100.1)
  p.input(0, file('c'), 100.2)
  assert.equal(p.at(100.2)?.card, 'c', 'current work replaces old presentation immediately')
  p.input(1, '{"changes":[{"value":{"path":"definitions/c/m.py","patches":[{"offset":0,"remove":2,"text":"new', 100.3)
  assert.equal(p.at(100.3)?.text, 'new')
  assert.equal(p.at(100.3)?.patch, true)
  p.input(1, '"},{"offset":5,"remove":3,"text":"', 100.4)
  assert.equal(p.at(100.4)?.text, '', 'an empty replacement cannot retain the previous patch')
  p.input(1, '"}]}}]}', 100.5)
  p.ended(false, 100.6)
  assert.equal(p.at(100.6), undefined)
  p.input(2, file('c'), 101)
  p.stop()
  assert.equal(p.at(101), undefined)
  p.input(3, '{"changes":[{"value_type":"RemoveSource","value":{"path":"definitions/c/m.py"}}]}', 102)
  assert.equal(p.at(102), undefined, 'a path without source content is not code being written')
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

// A Card that is not on the canvas: where there is room.
{
  const view = { x: 12, y: 52, width: 1000, height: 800 }
  const [w, h] = [PANEL.width, PANEL.height]
  const top = panelFreeFrame(1, view)
  assert.deepEqual(top, { x: view.x + view.width / 2 - w / 2, y: view.y, width: w, height: h })
  const bottom = panelFreeFrame(1, view, [top])
  assert.deepEqual(bottom, { x: top.x, y: view.y + view.height - h, width: w, height: h })
  const corner = panelFreeFrame(1, view, [top, bottom])
  assert.ok(corner.x === view.x && corner.y === view.y, 'a corner, covering nothing')
  assert.deepEqual(panelFreeFrame(1, view, [view]), top, 'nothing free: the first place all the same')
  assert.deepEqual(panelFreeFrame(1, view, [], 1), bottom, 'another panel does not take the same place')
}
console.log('live code behavior: passed')
