// Wires between ports close together take the short way; wires going back still go around.
import assert from 'node:assert/strict'
import { WireRouter } from '../src/model/canvasKit'

{
  const a = { x: 0, y: 0, width: 80, height: 50 } // the merge above
  const b = { x: -70, y: 70, width: 220, height: 90 } // the Card just under it
  const length = (pts: { x: number; y: number }[]) => pts.slice(1).reduce((s, p, i) => s + Math.hypot(p.x - pts[i].x, p.y - pts[i].y), 0)
  const down = new WireRouter('topToBottom', [a, b])
  for (const gap of [0, 1, 20, 23, 24, 25, 40]) {
    const r = new WireRouter('topToBottom', [a, { ...b, y: 50 + gap }]).route({ x: 40, y: 50 }, { x: 40, y: 50 + gap })
    assert.ok(length(r.pts) <= gap + 0.01, `straight across a gap of ${gap}: ${length(r.pts)}`)
  }
  const side = new WireRouter('leftToRight', [{ x: 0, y: 0, width: 50, height: 80 }, { x: 70, y: -70, width: 90, height: 220 }])
    .route({ x: 50, y: 40 }, { x: 70, y: 46 })
  assert.ok(length(side.pts) < 25, `short across, left to right: ${length(side.pts)}`)
  // Slightly offset ports stay within the gap, with no bulge past either port.
  const offset = down.route({ x: 40, y: 50 }, { x: 52, y: 70 })
  assert.ok(offset.pts.every((p) => p.y >= 50 - 0.01 && p.y <= 70 + 0.01), 'no bulge past the ports')
  // Going back, even by a little, goes around.
  const back = new WireRouter('topToBottom', [a, b]).route({ x: 40, y: 160 }, { x: 40, y: 0 })
  assert.ok(back.pts.some((p) => p.x < b.x || p.x > b.x + b.width), 'a feedback wire goes outside')
  const behind = new WireRouter('topToBottom', [a, b]).route({ x: 40, y: 51 }, { x: 40, y: 50 })
  assert.ok(length(behind.pts) > 50, 'upstream by a point is still upstream')
  // A short forward wire that would cross a Card goes around it, as before.
  const wall = { x: 100, y: 45, width: 60, height: 30 }
  const blocked = new WireRouter('topToBottom', [a, b, wall]).route({ x: 40, y: 50 }, { x: 220, y: 60 })
  const inside = (p: { x: number; y: number }) => p.x > wall.x + 2 && p.x < wall.x + wall.width - 2 && p.y > wall.y + 2 && p.y < wall.y + wall.height - 2
  assert.ok(!blocked.pts.some(inside), 'never through a Card')
  console.log('short wires: passed')
}
