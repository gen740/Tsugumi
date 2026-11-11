import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { CardGeometry } from '../src/model/canvasKit'
import { ProjectSession, TEMPLATES, definitionKind } from '../src/model/session'
import { get, parse, serialize } from '../src/shared/json'
import { CardNodeView } from '../src/ui/cardNode'

const session = new ProjectSession('/switch-test', 'switch-test', 'switch-test')
session.project = parse('{"name":"switch-test","canvas":{"cards":[],"connections":[]}}')
session.layout = parse('{}')
let saves = 0
session.scheduleSave = () => { saves++ }
const card = session.addCard('switch', [], { x: 0, y: 0 }, 'calibrate')
assert.ok(TEMPLATES.some(t => t.id === 'switch'))
assert.equal(definitionKind(session.cardDefinition(card)), 'switch')
assert.equal(session.sourcePaths.length, 0, 'A built-in Switch needs no schema or source file')
assert.deepEqual(session.portData(card), { inputs: [], outputs: [{ name: 'value', typeName: 'tsugumi.builtin.Condition', note: undefined }] })
assert.deepEqual(session.inlineParams(card), [{ name: 'value', type: 'bool', value: 'false' }])

const before = saves
session.setParam(card, 'value', true)
assert.equal(session.inlineParams(card)[0].value, 'true')
assert.ok(saves > before, 'Toggling schedules the normal project save')
const saved = parse(serialize(session.cardDefinition(card)!))
assert.equal(get((get(saved, 'params') as any[])[0], 'data'), true, 'The saved value is a boolean')
session.undo()
assert.equal(session.inlineParams(card)[0].value, 'false')
session.redo()
assert.equal(session.inlineParams(card)[0].value, 'true')

for (const flow of ['leftToRight', 'topToBottom'] as const) {
  for (const portGrid of [undefined, 20, 30]) {
    const data = { ...session.nodeData(card), portGrid }
    const geometry = new CardGeometry(data, flow)
    const width = portGrid ? 120 : 112
    const height = flow === 'leftToRight' && portGrid ? portGrid === 20 ? 40 : 60 : 38
    assert.deepEqual(geometry.size, { width, height })
    assert.deepEqual(geometry.port('value', true), flow === 'leftToRight' ? { x: width, y: height / 2 } : { x: width / 2, y: height })
    assert.equal(geometry.showLabels(false), false, 'The port type is available on hover, without widening the switch')
    const html = renderToStaticMarkup(<CardNodeView data={data} flow={flow} paramsEditable onParamCommit={() => {}} />)
    assert.match(html, /role="switch"/)
    assert.match(html, /aria-checked="true"/)
    assert.match(html, /aria-label="calibrate"/)
    assert.match(html, /value: bool/)
    assert.doesNotMatch(html, />ON<|>OFF</)
    const locked = renderToStaticMarkup(<CardNodeView data={data} flow={flow} paramsEditable={false} onParamCommit={() => {}} />)
    assert.match(locked, /role="switch"[^>]*disabled=""/)
  }
}

// Minimal definitions use the built-in default; the first click writes it.
session.updateCard(card, c => c.set('params', undefined))
assert.equal(session.inlineParams(card)[0].value, 'false')
session.setParam(card, 'value', true)
assert.equal(session.inlineParams(card)[0].value, 'true')
session.undo()
assert.equal(get(session.cardDefinition(card), 'params'), undefined)
assert.equal(session.inlineParams(card)[0].value, 'false')

console.log('Switch Card: template, save/undo, typed ports and accessible rendering passed')
