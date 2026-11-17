import { cardIds } from './cardIds'
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { CardGeometry } from '../src/model/canvasKit'
import { ProjectSession, TEMPLATES, definitionKind } from '../src/model/session'
import { get, parse, serialize } from '../shared/json'
import { CardNodeView } from '../src/ui/cardNode'

const session = cardIds(new ProjectSession('/switch-test', 'switch-test', 'switch-test'))
session.project = parse('{"name":"switch-test","canvas":{"cards":[],"connections":[]}}')
session.layout = parse('{}')
let operations = 0
const operate = session.operateProject
session.operateProject = async request => { operations++; return operate(request) }
const card = await session.addCard('switch', [], { x: 0, y: 0 }, 'calibrate')
assert.ok(TEMPLATES.some(t => t.id === 'switch'))
assert.equal(definitionKind(session.cardDefinition(card)), 'switch')
assert.equal(session.sourcePaths.length, 0, 'A built-in Switch needs no schema or source file')
assert.deepEqual(session.portData(card), { inputs: [], outputs: [{ name: 'value', typeName: 'tsugumi.builtin.Condition', note: undefined }] })
assert.deepEqual(session.inlineParams(card), [{ name: 'value', type: 'bool', value: 'false' }])

const before = operations
session.setParam(card, 'value', true)
await session.save()
assert.equal(session.inlineParams(card)[0].value, 'true')
assert.equal(operations, before + 1, 'Toggling submits one operation')
const saved = parse(serialize(session.cardDefinition(card)!))
assert.equal(get((get(saved, 'params') as any[])[0], 'data'), true, 'The saved value is a boolean')
await session.undo()
assert.equal(session.inlineParams(card)[0].value, 'false')
await session.redo()
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
await session.save()
assert.equal(session.inlineParams(card)[0].value, 'false')
session.setParam(card, 'value', true)
await session.save()
assert.equal(session.inlineParams(card)[0].value, 'true')
await session.undo()
assert.deepEqual(get(session.cardDefinition(card), 'params') ?? [], [])
assert.equal(session.inlineParams(card)[0].value, 'false')

console.log('Switch Card: template, operation/undo, typed ports and accessible rendering passed')

// Routing ports shrink with the body. At 20%, the old 16px target covered the
// start's title and the branch's centre, stealing a body drag to start a wire.
for (const flow of ['topToBottom', 'leftToRight'] as const) {
  for (const zoom of [1, 0.2]) {
    for (const kind of ['trigger', 'branch', 'fork', 'merge', 'collect', 'switch'] as const) {
      const data = { ...session.nodeData(card), kind,
        inputs: kind === 'trigger' || kind === 'switch' ? [] : [{ name: 'value' }, { name: 'condition' }],
        outputs: [{ name: 'a' }, { name: 'b' }] }
      const g = new CardGeometry(data, flow)
      const body = kind === 'trigger' ? { x: 55, y: 17 } : g.iconCenter(g.outlineRect)
      const hit = g.portHitSize(zoom)
      for (const output of [false, true]) {
        for (const port of output ? data.outputs : data.inputs) {
          const p = g.port(port.name, output)
          assert.ok(Math.abs(p.x - body.x) * zoom > hit / 2 || Math.abs(p.y - body.y) * zoom > hit / 2,
            `${kind} body is free of port targets at ${zoom}, ${flow}`)
        }
      }
      const html = renderToStaticMarkup(<CardNodeView data={data} flow={flow} zoom={zoom} />)
      assert.ok(html.includes(`width:${hit}px;height:${hit}px`), 'rendering uses the routing target size')
    }
  }
}

console.log('Routing Card body and port targets at full and small zoom: passed')
