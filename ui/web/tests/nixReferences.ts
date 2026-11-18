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
  nixReferences: [{ target: ['child', 'inner'], name: 'child-result', valid: true },
    { target: ['missing'], name: 'missing', valid: false }] }]])
assert.deepEqual(nixReferences({ resolution }).map(r => [r.source, r.target, r.expression, r.valid]), [
  ['sink', 'child/inner', 'inputs.child-result', true],
  ['sink', 'missing', 'inputs.missing', false],
])
const card = (path: string[]): ResolvedCard => ({ path, inputs: [], outputs: [], diagnostics: [], nixReferences: [] })
for (const k of ['child', 'child/inner']) resolution.set(k, card(k.split('/')))
assert.equal(nixLinkCount({ resolution }, []), 1, 'a nested target meets its visible container')
assert.equal(nixLinkCount({ resolution }, ['child']), 0, 'the source is not at this level: nothing to draw')
resolution.get('sink')!.nixReferences = []
assert.equal(nixReferences({ resolution }).length, 0, 'removed references disappear without stale cache')
const curve = nixCurve({ x: 0, y: 0, width: 100, height: 80 }, { x: 300, y: 0, width: 100, height: 80 })
assert.ok(pathToScreen(curve, { x: 20, y: 30, zoom: 2 }).startsWith('M 220.00,110.00'), 'overlay follows pan and zoom')
console.log('Nix reference navigation: passed')
