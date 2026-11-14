import assert from 'node:assert/strict'
import { Builder, ByteBuffer } from 'flatbuffers'
import { R, RPCClient, RequestPayload, s as text } from '../src/shared/rpc'
import { clone, get, parse, serialize } from '../src/shared/json'

const { ProjectSession } = await import('../src/model/session')
const requests: { name: string; prefix: string }[] = []
const ids = ['aggregate-7k2q', 'aggregate-9m3r', 'inside-8p4s', 'child-6t5v']
const allocator = new RPCClient({ close() {}, async call(frame) {
  const message = R.Message.getRootAsMessage(new ByteBuffer(frame)).unpack()
  const request = message.payload as R.RequestT
  assert.equal(request.valueType, RequestPayload.CardIdRequest)
  const value = request.value as R.CardIdRequestT
  requests.push({ name: text(value.name), prefix: text(value.projectPath) })
  const result = new R.CardIdResultT(ids[requests.length - 1], value.name)
  const reply = new R.MessageT(message.requestId, R.Payload.Reply, new R.ReplyT(R.ReplyPayload.CardIdResult, result))
  const builder = new Builder(512)
  builder.finish(reply.pack(builder), 'TSG1')
  return builder.asUint8Array().slice()
} })
const session = new ProjectSession('/p', 'k', 'p')
session.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
session.confirmedProject = clone(session.project)
session.layout = parse('{}')
session.confirmedLayout = parse('{}')
session.scheduleSave = () => {}
session.save = async () => { await (session as unknown as { saveOnce(): Promise<void> }).saveOnce() }
let saves = 0
session.client = {
  cardId: allocator.cardId.bind(allocator),
  async save(_rev: bigint, _op: string, _origin: string, changes: R.DocumentChangeT[]) {
    saves++
    // This boundary echoes confirmed documents; no identity moves exist.
    for (const change of changes) {
      if (change.valueType === R.DocumentChangeValue.PutDefinition) {
        const definition = change.value as R.PutDefinitionT
        session.confirmedDefinitions.set(text(definition.id), parse(text(definition.cardJson)))
        session.confirmedDefinitionMeta.set(text(definition.id), parse(text(definition.metaJson)))
      }
    }
    session.confirmedProject = clone(session.project)
    session.confirmedLayout = clone(session.layout)
    return {}
  },
} as never
session.connected = true
const a = await session.addCard('draft', [], { x: 0, y: 0 }, 'Aggregate')
const b = await session.addCard('draft', [], { x: 400, y: 0 }, 'Aggregate')
assert.equal(a, ids[0]); assert.equal(b, ids[1])
assert.notEqual(a, b)
assert.equal(session.displayName(a), session.displayName(b))
await session.connect([a, 'out'], [b, 'in'], [])
await session.save()
const project = serialize(session.project), layout = serialize(session.layout), dir = session.definitionDir(a)
session.displays.set(a, { runId: 'past', invocation: 1n, revision: 1n, value: { kind: 'text', text: 'kept' } })
await session.renameCard(a, '集計 / 同じ名前')
assert.equal(session.displayName(a), '集計 / 同じ名前')
assert.equal(session.nodeData(a).title, '集計 / 同じ名前')
assert.equal(session.nodeData(a, { meta: parse('{"name":"Preview"}') }).title, 'Preview')
assert.equal(session.nodeData(a, { meta: parse('{"name":""}') }).title, a)
assert.equal(serialize(session.project), project)
assert.equal(serialize(session.layout), layout)
assert.equal(session.definitionDir(a), dir)
assert.equal(session.displays.get(a)?.runId, 'past')
assert.equal(text(requests[0].name), 'Aggregate')
assert.ok(saves >= 2)

// A Card inside an unsaved Card in Card asks the same core for its scope's id.
session.edit(s => {
  s.appendCard('outer', [])
  s.setCardDefinition('outer', { card: parse('{}'), meta: parse('{"name":"Outer"}'), link: parse('{"external":"nested"}') })
  s.createExternal('externals/nested/', { project: parse('{"name":"nested","canvas":{"cards":[],"connections":[]}}'), layout: parse('{}'), definitions: new Map() })
})
const inner = await session.addCard('draft', ['outer'], { x: 0, y: 0 }, 'Inside')
assert.equal(inner, `outer/${ids[2]}`)
assert.deepEqual(requests[2], { name: 'Inside', prefix: 'externals/nested/' })
assert.equal(session.displayPath(['outer', ids[2]]), 'Outer/Inside')
assert.equal(get(session.cardMeta(inner), 'name'), 'Inside')
console.log('Card identity: core ids, duplicate names, name-only edits, exact wires and nested scopes passed')
