// Only open live viewers subscribe to displays.
import assert from 'node:assert/strict'
import { Builder, ByteBuffer } from 'flatbuffers'
import { D, R, RPCClient, RequestPayload, cardPath, type Transport } from '../src/wire/rpc'
import type { ViewMessage } from '../src/model/liveViews'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')

const resolved = (id: string) => ({ card: { segments: [id] }, inputs: [], outputs: [], diagnostics: [], nixReferences: [] }) as unknown as R.ResolvedCardT
const definition = (id: string, card: string) => Object.assign(new R.DefinitionT(), { id, cardJson: card })
const delta = (rev: bigint, fields: Partial<R.DocumentDeltaT>) =>
  Object.assign(new R.DocumentDeltaT(), { baseRev: rev, rev: rev + 1n, resolution: [resolved('a')], diagnostics: [], ...fields })

const session = () => {
  const s = new ProjectSession('/p', 'k', 'p')
  const project = '{"name":"t","canvas":{"cards":[{"id":"a"}],"connections":[]}}'
  const asked: string[][] = []
  s.client = { setVisible: async (_sub: unknown, cards: string[][]) => { asked.push(cards.map((c) => c.join('/'))) } } as never
  const inner = s as unknown as { subscription: string; applyDelta(d: R.DocumentDeltaT): void; apply(snapshot: R.ProjectSnapshotT, reset: boolean): void }
  inner.subscription = 'sub'
  inner.apply(Object.assign(new R.ProjectSnapshotT(), { rev: 1n, projectJson: project, layout: [],
    definitions: [definition('a', '{"type":"draft"}')], resolution: [resolved('a')] }))
  return { s, asked, apply: (d: R.DocumentDeltaT) => inner.applyDelta(d) }
}

const { s, asked, apply } = session()
apply(delta(1n, { definitionsUpsert: [definition('a', '{"type":"code"}')] }))
assert.equal(s.kind('a'), 'code')
assert.deepEqual(asked, [], 'the canvas does not subscribe when a Draft becomes code')

const closeA = s.observeDisplay('a')
const closeSecondA = s.observeDisplay('a')
const closeB = s.observeDisplay('nested/b')
closeA()
closeB()
closeSecondA()
assert.deepEqual(asked, [['a'], ['a', 'nested/b'], ['a'], []])


function frame(message: R.MessageT): Uint8Array {
  const builder = new Builder(256)
  builder.finish(message.pack(builder), 'TSG1')
  return builder.asUint8Array().slice()
}

const { s: live } = session()
const requests: R.CardViewReadRequestT[] = []
const source = new TextEncoder().encode('<!doctype html><p>海</p>')
const wire: Transport = { close() {}, async call(bytes) {
  const request = R.Message.getRootAsMessage(new ByteBuffer(bytes)).unpack()
  const { valueType, value } = request.payload as R.RequestT
  if (valueType === RequestPayload.ViewSetVisibleRequest)
    return frame(new R.MessageT(request.requestId, R.Payload.Reply, new R.ReplyT(R.ReplyPayload.Ack, new R.AckT())))
  assert.equal(valueType, RequestPayload.CardViewReadRequest)
  const read = value as R.CardViewReadRequestT
  requests.push(read)
  const offset = Number(read.offset)
  const page = [...source.subarray(offset, offset + 2)]
  const part = Object.assign(new R.FileContentT(), {
    name: read.name, hash: '/nix/store/view', sizeBytes: BigInt(source.length), offset: BigInt(offset),
    nextOffset: BigInt(offset + page.length), bytes: page, eof: offset + page.length >= source.length,
  })
  return frame(new R.MessageT(request.requestId, R.Payload.Reply, new R.ReplyT(R.ReplyPayload.FileContent, part)))
} }
const client = new RPCClient(wire)
live.client = client
const loop: (() => void)[] = []
page.requestAnimationFrame = (fn: () => void) => { loop.push(fn); return loop.length }
page.cancelAnimationFrame = () => {}
;(live as unknown as { listen(client: RPCClient): void }).listen(client)
let seq = 0n
function notify(type: R.NotificationPayload, value: R.NotificationT['value']) {
  const message = new R.NotificationT('sub', ++seq, type, value)
  const packet = frame(new R.MessageT(null, R.Payload.Notification, message))
  wire.onEvent!(packet)
  packet.fill(0) // The transport can reuse this buffer before the UI batch runs.
}
const flush = () => loop.shift()?.()
const card = cardPath(['a'])
const declarations = [
  new D.CardViewT('first', 'html', '/nix/store/first'),
  new D.CardViewT('second', 'html', '/nix/store/second'),
  new D.CardViewT('future', 'dawn', '/nix/store/future'),
]
const declare = (views: D.CardViewT[]) =>
  notify(R.NotificationPayload.CardViewsUpdated, new R.CardViewsUpdatedT(card, views))
const send = (name: string, bytes: number[], run: string | null = 'run-a', source = card) =>
  notify(R.NotificationPayload.DisplayUpdated, new R.DisplayUpdatedT(new D.DisplayT(source, run, name, bytes)))
const control = (state: D.DisplayRunState) =>
  notify(R.NotificationPayload.DisplayRunStateChanged, new R.DisplayRunStateChangedT(card, 'run-a', state))

send('unobserved', [])
flush()
assert.equal(live.liveViews.size, 0)
const close = live.observeDisplay('a')
declare(declarations)
flush()
const entry = live.liveViews.get('a')!
assert.deepEqual(entry.views.map(v => [v.index, v.declaration.name]), [[0, 'first'], [1, 'second'], [2, 'future']])
const [first, second] = entry.views
assert.equal(first.latest, undefined)
const firstMessages: ViewMessage[] = [], secondMessages: ViewMessage[] = []
const offFirst = first.listen(entry.activeRun, event => firstMessages.push(event))
const offSecond = second.listen(entry.activeRun, event => secondMessages.push(event))
control(D.DisplayRunState.started)
send('first', [0, 255, 128])
send('second', [1, 0, 2])
send('unexpected', [42])
send('first', [7], 'run-a', cardPath(['different']))
control(D.DisplayRunState.ended)
send('second', [], null)
flush()
assert.deepEqual(firstMessages, [
  { type: 'run', state: 'started' },
  { type: 'data', bytes: Uint8Array.of(0, 255, 128) },
  { type: 'run', state: 'ended' },
])
assert.deepEqual(secondMessages, [
  { type: 'run', state: 'started' },
  { type: 'data', bytes: Uint8Array.of(1, 0, 2) },
  { type: 'run', state: 'ended' },
  { type: 'data', bytes: new Uint8Array() },
])
assert.equal(entry.activeRun, undefined)
assert.deepEqual([...entry.unknown], ['unexpected'])
declare(declarations)
flush()
assert.equal(entry.views[0], first, 'an unchanged blob keeps its frame and bytes')
declare([new D.CardViewT('first', 'html', '/nix/store/rebuilt')])
flush()
assert.equal(entry.views.length, 1)
assert.equal(entry.views[0].latest, undefined, 'a rebuilt blob starts with an empty view')
declare([new D.CardViewT('twin', 'html', '/nix/store/a'), new D.CardViewT('twin', 'html', '/nix/store/b')])
flush()
assert.deepEqual([...entry.ambiguous], ['twin'], 'a repeated name is reported')
send('twin', [5])
flush()
assert.ok(entry.views.every(v => v.latest === undefined), 'data for a repeated name reaches neither view')
assert.equal(entry.unknown.has('twin'), false)
const closeAgain = live.observeDisplay('a')
close()
assert.equal(live.liveViews.get('a'), entry)
const broken = Object.assign(new D.FailureT(), { kind: D.FailureKind.nix_failed, diagnostic: 'views are broken' })
notify(R.NotificationPayload.CardViewsUpdated, Object.assign(new R.CardViewsUpdatedT(card, []), { failure: broken }))
flush()
assert.equal(entry.failure, 'views are broken', 'a failed evaluation is kept apart from an empty list')
assert.equal(entry.views.length, 0)
declare([])
flush()
assert.equal(entry.failure, undefined, 'an evaluated empty list has no failure')
assert.equal(entry.views.length, 0, 'an empty list clears all declarations')
assert.equal(entry.ambiguous.size, 0)
closeAgain()
assert.equal(live.liveViews.has('a'), false)
assert.equal(entry.unknown.size, 0)
offFirst()
offSecond()

const controller = new AbortController()
assert.equal(await client.readCardView(['a'], 1, 'first', controller.signal), new TextDecoder().decode(source))
assert.ok(requests.length > 1)
assert.deepEqual(requests.map(r => r.offset), requests.map((_, i) => BigInt(i * 2)))
assert.ok(requests.every(r => r.name === 'first' && r.index === 1 && r.card?.segments[0] === 'a'))
controller.abort()
await assert.rejects(client.readCardView(['a'], 1, 'first', controller.signal), { name: 'AbortError' })
console.log('live views: visibility, declarations, binary routing, control order, disposal and chunked reads passed')
