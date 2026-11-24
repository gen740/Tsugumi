import assert from 'node:assert/strict'
import { nixCurve, nixLinkCount, nixReferences, visibleNixCard } from '../src/model/nixReferences'
import { pathToScreen } from '../src/model/canvas'
import { ResolvedCard } from '../src/model/session'

const visible = new Set(['sink', 'child'])
assert.equal(visibleNixCard('child/inner', [], visible), 'child')
assert.equal(visibleNixCard('missing', [], visible), undefined)
assert.equal(visibleNixCard('sibling/inner', ['child'], new Set(['child/inner'])), undefined)
assert.equal(visibleNixCard('child/inner', ['child'], new Set(['child/inner'])), 'child/inner')
const resolution = new Map<string, ResolvedCard>([['sink', { path: ['sink'], inputs: [], outputs: [], diagnostics: [],
  nixReferences: [{ target: ['child', 'inner'], name: 'child result' },
    { target: ['missing'], name: 'missing' }] }]])
const byTarget = new Map(nixReferences({ resolution }).map(ref => [ref.target, ref]))
assert.deepEqual(new Set(byTarget.keys()), new Set(['child/inner', 'missing']))
assert.equal(byTarget.get('child/inner')?.source, 'sink')
assert.equal(byTarget.get('child/inner')?.expression, 'inputs."child result"')
const card = (path: string[]): ResolvedCard => ({ path, inputs: [], outputs: [], diagnostics: [], nixReferences: [] })
for (const k of ['child', 'child/inner']) resolution.set(k, card(k.split('/')))
assert.equal(nixLinkCount({ resolution }, []), 1, 'a nested target meets its visible container')
assert.equal(nixLinkCount({ resolution }, ['child']), 0, 'the source is not at this level: nothing to draw')
resolution.get('sink')!.nixReferences = []
assert.equal(nixReferences({ resolution }).length, 0, 'removed references disappear without stale cache')
const curve = nixCurve({ x: 0, y: 0, width: 100, height: 80 }, { x: 300, y: 0, width: 100, height: 80 })
const camera = { x: 20, y: 30, zoom: 2 }
const points = (path: string) => [...path.matchAll(/(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)/g)].map(([, x, y]) => [Number(x), Number(y)])
const world = points(curve), screen = points(pathToScreen(curve, camera))
for (const [start, end] of [[world[0], screen[0]], [world.at(-1)!, screen.at(-1)!]]) {
  assert.ok(Math.abs(end[0] - (start[0] * camera.zoom + camera.x)) < 0.01)
  assert.ok(Math.abs(end[1] - (start[1] * camera.zoom + camera.y)) < 0.01)
}
console.log('Nix reference navigation: passed')
