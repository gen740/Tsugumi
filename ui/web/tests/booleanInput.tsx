import { cardIds } from './cardIds'
import assert from 'node:assert/strict'
import { Builder } from 'flatbuffers'
import TestRenderer from 'react-test-renderer'
import { L } from '../src/i18n'
import { CardGeometry } from '../src/model/canvasKit'
import { ProjectSession, TEMPLATES, definitionKind } from '../src/model/session'
import { get, parse } from '../shared/json'
import { CardNodeView, ParamFieldView } from '../src/ui/cardNode'
import { CardEditorModel } from '../src/ui/cardSettings'
import * as H from '../shared/gen/tsugumi/host'
import { R, cardPath } from '../src/wire/rpc'

const session = cardIds(new ProjectSession('/input-test', 'input-test', 'input-test'))
session.project = parse('{"name":"input-test","canvas":{"cards":[],"connections":[]}}')
session.layout = parse('{}')
let operations = 0
const operate = session.operateProject
const resolved = (card: string, value: string) =>
  Object.assign(new R.ResolvedCardT(), { card: cardPath([card]), parameters: [new R.CardParameterT('value', 'bool', value)] })
session.operateProject = async request => {
  operations++
  if (request.bodyType !== H.ProjectOperationBody.ProjectRewriteParamLiteral) return operate(request)
  const param = request.body as H.ProjectRewriteParamLiteralT
  assert.equal(param.expectedRev, session.rev)
  assert.equal(param.name, 'value')
  assert.deepEqual(param.card?.segments, ['calibrate'])
  const delta = Object.assign(new R.DocumentDeltaT(), { baseRev: session.rev, rev: session.rev + 1n,
    resolution: [resolved('calibrate', String(param.literal))] })
  const builder = new Builder()
  builder.finish(new H.ProjectEditsT().pack(builder))
  return Object.assign(new R.SaveResultT(), { rev: delta.rev, delta, operationData: [...builder.asUint8Array()] })
}
const preset = TEMPLATES.find(t => t.id === 'input_bool')
assert.ok(preset)
const card = await session.addCard(preset.id, [], { x: 0, y: 0 }, 'calibrate')
const merge = await session.addCard('merge', [], { x: 0, y: 100 })
assert.equal(definitionKind(session.cardDefinition(card)), 'input')
await session.connect([card, 'value'], [merge, 'a'], [])
assert.ok(session.connections([]).some(wire =>
  get(wire, 'from', 'card') === card && get(wire, 'from', 'port') === 'value' &&
  get(wire, 'to', 'card') === merge && get(wire, 'to', 'port') === 'a'))

session.resolution.get(card)!.parameters = [{ name: 'value', type: 'bool', value: false }]
assert.deepEqual(session.inlineParams(card), [{ name: 'value', type: 'bool', value: 'false' }])
const before = operations
let pending: Promise<void> | undefined
const render = (editable: boolean) => <ParamFieldView param={session.inlineParams(card)[0]} editable={editable}
  commit={value => {
    pending = (async () => {
      await session.setParam(card, 'value', value === 'true', session.rev)
    })()
    return pending
  }} />
const view = TestRenderer.create(render(true))
const checkbox = () => view.root.findByProps({ type: 'checkbox', 'aria-label': 'value' })
assert.equal(checkbox().props.checked, false)
checkbox().props.onChange({ target: { checked: true } })
assert.ok(pending)
await pending
view.update(render(true))
assert.equal(checkbox().props.checked, true)
view.update(render(false))
assert.equal(checkbox().props.disabled, true)
view.unmount()
assert.equal(operations, before + 1, 'Toggling submits one operation')
assert.equal(get(session.cardDefinition(card), 'params'), undefined)
assert.equal(session.inlineParams(card)[0].value, 'true')

const draft = await session.addCard('draft', [], { x: 100, y: 0 })
const draftEditor = new CardEditorModel(session, draft)
draftEditor.implementAs('input')
draftEditor.implementAs(preset.id)
assert.equal(draftEditor.implementation, preset.id)
assert.equal(await draftEditor.apply(), true)
assert.equal(await session.sourceText(`definitions/${draft}/params.nix`), '{ value = false; }\n')
assert.equal(get(session.cardDefinition(draft), 'params'), undefined)
const outputSchema = (async (id: string) => (await session.sourceText(`definitions/${id}/_outputs.fbs`))
  .replace(/^namespace .*;\n/, ''))
assert.equal(await outputSchema(draft), await outputSchema(card))

const customDraft = await session.addCard('draft', [], { x: 200, y: 0 })
const original = 'table Value { value:bool; } table Outputs { custom:Value (required); } root_type Outputs;'
const sourceEditor = new CardEditorModel(session, customDraft)
sourceEditor.createFile('_outputs.fbs', original)
assert.equal(await sourceEditor.apply(), true)

const existingEditor = new CardEditorModel(session, customDraft)
const schemaPath = existingEditor.outputsPath!
existingEditor.implementAs('input')
existingEditor.implementAs(preset.id)
assert.equal(await existingEditor.apply(), true)
assert.equal(await session.sourceText(schemaPath), original)

const freshDraft = await session.addCard('draft', [], { x: 300, y: 0 })
const freshEditor = new CardEditorModel(session, freshDraft)
freshEditor.implementAs('input')
const localSchema = original.replace('custom:', 'unsaved:')
freshEditor.setText(freshEditor.outputsPath!, localSchema)
freshEditor.implementAs(preset.id)
assert.equal(await freshEditor.apply(), true)
assert.equal(await session.sourceText(freshEditor.outputsPath!), localSchema)

// Routing ports shrink with the body. At 20%, the old 16px target covered the
// start's title and the routing Card's centre, stealing a body drag to start a wire.
for (const flow of ['topToBottom', 'leftToRight'] as const) {
  for (const zoom of [1, 0.2]) {
    for (const kind of ['trigger', 'merge', 'collect'] as const) {
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
