import assert from 'node:assert/strict'
import { readViewState, writeViewState } from '../src/shared/clientViewState'

const good = { scope: ['child'], x: 1.5, y: -2, zoom: 1.25 }
const bad = [0, -1, NaN, Infinity].map(zoom => ({ ...good, zoom }))
const text = writeViewState({ scope: [], cameras: [good, ...bad, { ...good, x: NaN }], selected_cards: [['a']] })
assert.deepEqual(readViewState(text), { scope: [], cameras: [good], selected_cards: [['a']] })
assert.deepEqual(readViewState('{"scope":[],"cameras":[null,{"scope":[],"x":0,"y":0,"zoom":-1}],"selected_cards":[3,["a"]],"extension":true}'),
  { scope: [], cameras: [], selected_cards: [['a']] })
for (const value of ['null', '[]', '{}', 'invalid']) assert.equal(readViewState(value), undefined)
console.log('client view state: finite coordinates and positive zoom checked at the client')
