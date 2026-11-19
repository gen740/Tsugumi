import { cardIds } from './cardIds'
import assert from 'node:assert/strict'
import TestRenderer from 'react-test-renderer'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { L } from '../src/i18n'
import { CardGeometry } from '../src/model/canvasKit'
import { ProjectSession, TEMPLATES, definitionKind } from '../src/model/session'
import { get, parse, serialize } from '../shared/json'
import { CardNodeView } from '../src/ui/cardNode'

const session = cardIds(new ProjectSession('/input-test', 'input-test', 'input-test'))
session.project = parse('{"name":"input-test","canvas":{"cards":[],"connections":[]}}')
session.layout = parse('{}')
let operations = 0
const operate = session.operateProject
session.operateProject = async request => { operations++; return operate(request) }
const card = await session.addCard('input', [], { x: 0, y: 0 }, 'calibrate')
assert.ok(TEMPLATES.some(t => t.id === 'input'))
assert.equal(definitionKind(session.cardDefinition(card)), 'input')
// A Boolean Input uses the same parameter and save flow as other Inputs.
session.updateCard(card, c => {
  c.set('params', parse('[{"name":"value","type":"bool","data":false}]'))
})
await session.save()
assert.deepEqual(session.inlineParams(card), [{ name: 'value', type: 'bool', value: 'false' }])

const before = operations
await session.applyMutation(x => x.updateParam(card, 'value', true))
await session.save()
assert.equal(session.inlineParams(card)[0].value, 'true')
assert.equal(operations, before + 1, 'Toggling submits one operation')
const saved = parse(serialize(session.cardDefinition(card)!))
assert.equal(get((get(saved, 'params') as any[])[0], 'data'), true, 'The saved value is a boolean')
await session.undo()
assert.equal(session.inlineParams(card)[0].value, 'false')
await session.redo()
assert.equal(session.inlineParams(card)[0].value, 'true')

const data = session.nodeData(card)
const editable = renderToStaticMarkup(<CardNodeView data={data} flow="topToBottom" paramsEditable onParamCommit={() => {}} />)
assert.match(editable, /type="checkbox"[^>]*checked=""/)
const locked = renderToStaticMarkup(<CardNodeView data={data} flow="topToBottom" paramsEditable={false} onParamCommit={() => {}} />)
assert.match(locked, /type="checkbox"[^>]*disabled=""/)

// Routing ports shrink with the body. At 20%, the old 16px target covered the
// start's title and the branch's centre, stealing a body drag to start a wire.
for (const flow of ['topToBottom', 'leftToRight'] as const) {
  for (const zoom of [1, 0.2]) {
    for (const kind of ['trigger', 'branch', 'merge', 'collect'] as const) {
      const data = { ...session.nodeData(card), kind,
        inputs: kind === 'trigger' ? [] : [{ name: 'value' }, { name: 'condition' }],
        outputs: [{ name: 'a' }, { name: 'b' }] }
      const g = new CardGeometry(data, flow)
      const body = g.iconCenter(g.outlineRect)
      const hit = g.portHitSize(zoom)
      for (const output of [false, true]) {
        for (const port of output ? data.outputs : data.inputs) {
          const p = g.port(port.name, output)
          assert.ok(Math.abs(p.x - body.x) * zoom > hit / 2 || Math.abs(p.y - body.y) * zoom > hit / 2,
            `${kind} body is free of port targets at ${zoom}, ${flow}`)
        }
      }
      const started: { port: string; output: boolean }[] = []
      let moved = false
      const rendered = TestRenderer.create(<CardNodeView data={data} flow={flow} zoom={zoom}
        onPortDown={(port, output) => started.push({ port, output })} onPointerDown={() => { moved = true }} />)
      const target = rendered.root.findAll(node =>
        node.type === 'div' && node.props.onPointerDown && node.props.title?.includes('a') &&
        node.props.title?.startsWith(L('出力 ') + 'a'))[0]
      assert.ok(target, `${kind} exposes an output wiring target`)
      let stopped = false
      target.props.onPointerDown({ button: 0, stopPropagation() { stopped = true } })
      assert.deepEqual(started, [{ port: 'a', output: true }])
      assert.ok(stopped, 'a wiring gesture does not start a body drag')
      rendered.root.findByProps({ role: 'group' }).props.onPointerDown({ button: 0 })
      assert.ok(moved, 'a Card body still starts a drag')
      rendered.unmount()
    }
  }
}

console.log('Routing Card body and port targets at full and small zoom: passed')
