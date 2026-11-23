import assert from 'node:assert/strict'
import { readViewState, writeViewState } from '../shared/clientViewState'

const good = { scope: ['child'], x: 1.5, y: -2, zoom: 1.25 }
const bad = [0, -1, NaN, Infinity].map(zoom => ({ ...good, zoom }))
const text = writeViewState({ scope: [], cameras: [good, ...bad, { ...good, x: NaN }], selected_cards: [['a']] })
assert.deepEqual(readViewState(text), { scope: [], cameras: [good], selected_cards: [['a']] })
assert.equal(readViewState([]), undefined)
console.log('client view state: finite coordinates and positive zoom checked at the client')
