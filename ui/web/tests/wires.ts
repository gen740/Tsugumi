import assert from 'node:assert/strict'
import { WireRouter } from '../src/model/canvasKit'

const upper = { x: 0, y: 0, width: 80, height: 50 }
const lower = { x: -70, y: 70, width: 220, height: 90 }
const down = new WireRouter([upper, lower])
const from = { x: 40, y: upper.y + upper.height }
const to = { x: 52, y: lower.y }
const forward = down.route(from, to).pts
assert.deepEqual(forward[0], from)
assert.deepEqual(forward.at(-1), to)
assert.ok(forward.every((p) => p.y >= from.y && p.y <= to.y), 'a short wire stays between the ports')

const back = down.route({ x: 40, y: lower.y + lower.height }, { x: 40, y: upper.y })
assert.ok(back.pts.some((p) => p.x <= lower.x || p.x >= lower.x + lower.width), 'a feedback wire goes outside')

const wall = { x: 100, y: 45, width: 60, height: 30 }
const start = { x: 40, y: 50 }
const end = { x: 220, y: 60 }
const blocked = new WireRouter([wall]).route(start, end).pts
assert.deepEqual(blocked[0], start)
assert.deepEqual(blocked.at(-1), end)
for (const [index, point] of blocked.slice(1).entries()) {
  const prev = blocked[index]
  // A segment crossing the centerline inside the rectangle cuts through the obstacle.
  const middle = wall.x + wall.width / 2
  if (Math.min(prev.x, point.x) < middle && middle <= Math.max(prev.x, point.x)) {
    const y = prev.y + (point.y - prev.y) * (middle - prev.x) / (point.x - prev.x)
    assert.ok(y <= wall.y || y >= wall.y + wall.height, 'a forward wire detours around a Card')
  }
}
console.log('short wires: passed')
