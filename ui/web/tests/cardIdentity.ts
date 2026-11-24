import { cardFixture, projectFixture } from './documentFixture'
import { hostEdits } from './cardIds'
import assert from 'node:assert/strict'
import { Builder, ByteBuffer } from 'flatbuffers'
import { R, RPCClient, RequestPayload, s as text } from '../src/wire/rpc'
import { clone, get, parse, serialize, str } from '../shared/json'

const { ProjectSession } = await import('../src/model/session')
const requests: { name: string; prefix: string }[] = []
const ids = ['aggregate', 'aggregate_2', 'inside']
const allocator = new RPCClient({ close() {}, async call(frame) {
  const message = R.Message.getRootAsMessage(new ByteBuffer(frame)).unpack()
  const request = message.payload as R.RequestT
  assert.equal(request.valueType, RequestPayload.CardIdRequest)
  const value = request.value as R.CardIdRequestT
  requests.push({ name: text(value.stem), prefix: text(value.projectPath) })
  const result = new R.CardIdResultT(ids[requests.length - 1])
  const reply = new R.MessageT(message.requestId, R.Payload.Reply, new R.ReplyT(R.ReplyPayload.CardIdResult, result))
  const builder = new Builder(512)
  builder.finish(reply.pack(builder), 'TSG1')
  return builder.asUint8Array().slice()
} })
const session = hostEdits(new ProjectSession('/p', 'k', 'p'))
session.project = parse(projectFixture({"name":"t","canvas":{"cards":[],"connections":[]}}))
session.confirmedProject = clone(session.project)
session.layout = parse('{}')
session.confirmedLayout = parse('{}')
let saves = 0
const operate = session.operateProject
session.operateProject = async request => { saves++; return operate(request) }
Object.assign((session as any).client, { cardId: allocator.cardId.bind(allocator) })
session.connected = true
const a = await session.addCard('draft', [], { x: 0, y: 0 }, 'Aggregate')
const b = await session.addCard('draft', [], { x: 400, y: 0 }, 'Aggregate')
assert.equal(a, ids[0]); assert.equal(b, ids[1])
assert.notEqual(a, b)
assert.equal(session.displayName(a), str(get(session.cardMeta(a), 'title')))
assert.equal(session.displayName(b), str(get(session.cardMeta(b), 'title')))
await session.connect([a, 'out'], [b, 'in'], [])
await session.save()
const project = serialize(session.project), layout = serialize(session.layout), dir = session.definitionDir(a)
const closeView = session.liveViews.observe(a, () => {})
const views = session.liveViews.get(a)!
views.declare([{ name: 'chart', type: 'html', blob: '/nix/store/chart' }])
const originalView = views.views[0]
await session.applyMutation(x => x.updateMeta(a, m => {
  m.set('title', '集計')
  m.set('description', '集計の説明')
}))
assert.equal(session.displayName(a), '集計')
assert.equal(session.nodeData(a).title, '集計')
assert.equal(session.cardDescription(a), '集計の説明')
assert.equal(session.nodeData(a, { meta: parse('{"title":"Preview"}') }).title, 'Preview')
assert.equal(session.nodeData(a, { meta: parse('{"title":""}') }).title, '集計')
assert.equal(serialize(session.project), project)
assert.equal(serialize(session.layout), layout)
assert.equal(session.definitionDir(a), dir)
assert.equal(views.views[0], originalView)
closeView()
assert.equal(text(requests[0].name), 'aggregate')
assert.ok(saves >= 2)

// Nested scopes use the same allocator through the operation transport.
session.confirmedDefinitions.set('outer', parse(cardFixture('subgraph')))
const projectValue = JSON.parse(serialize(session.project))
projectValue.canvas.cards.push({ id: 'outer' })
session.project = parse(JSON.stringify(projectValue))
session.confirmedDefinitionMeta.set('outer', parse('{"title":"Outer","description":""}'))
session.externals.set('externals/outer/', { project: parse(projectFixture({ name: 'nested', boundary: { inputs: [], outputs: [] } })), layout: parse('{}'), definitions: new Map() })
const inner = await session.addCard('draft', ['outer'], { x: 0, y: 0 }, 'Inside')
await new Promise<void>(resolve => setImmediate(resolve)) // Read the external documents from the confirmed delta.
assert.equal(inner, `outer/${ids[2]}`)
assert.deepEqual(requests[2], { name: 'inside', prefix: 'externals/outer/' })
assert.equal(session.displayPath(['outer', ids[2]]), 'Outer/' + session.displayName(inner))
assert.equal(session.displayName(inner), str(get(session.cardMeta(inner), 'title')))
console.log('Card identity: core ids, unique ids, description edits, exact wires and nested scopes passed')
