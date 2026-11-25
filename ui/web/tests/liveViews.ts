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

function frame(message: R.MessageT): Uint8Array {
  const builder = new Builder(256)
  builder.finish(message.pack(builder), 'TSG1')
  return builder.asUint8Array().slice()
}

const live = new ProjectSession('/p', 'k', 'p')
const inner = live as unknown as { subscription: string; listen(client: RPCClient): void }
inner.subscription = 'sub'
const reads: R.CardViewReadRequestT[] = []
const credits: R.ViewCreditRequestT[] = []
const reverse: R.ViewSendRequestT[] = []
const closes: R.ViewCloseRequestT[] = []
const source = new TextEncoder().encode('<!doctype html><p>海</p>')
const wire: Transport = { close() {}, async call(bytes) {
  const request = R.Message.getRootAsMessage(new ByteBuffer(bytes)).unpack()
  const { valueType, value } = request.payload as R.RequestT
  const reply = (type: R.ReplyPayload, value: R.ReplyT['value']) =>
    frame(new R.MessageT(request.requestId, R.Payload.Reply, new R.ReplyT(type, value)))
  switch (valueType) {
    case RequestPayload.ViewCreditRequest: credits.push(value as R.ViewCreditRequestT); break
    case RequestPayload.ViewSendRequest: reverse.push(value as R.ViewSendRequestT); break
    case RequestPayload.ViewCloseRequest: closes.push(value as R.ViewCloseRequestT); break
    case RequestPayload.CardViewReadRequest: {
      const read = value as R.CardViewReadRequestT
      reads.push(read)
      const offset = Number(read.offset)
      const bytes = [...source.subarray(offset, offset + 2)]
      return reply(R.ReplyPayload.FileContent, Object.assign(new R.FileContentT(), {
        name: 'same-view', hash: '/nix/store/view', sizeBytes: BigInt(source.length), offset: BigInt(offset),
        nextOffset: BigInt(offset + bytes.length), bytes, eof: offset + bytes.length >= source.length,
      }))
    }
    default: assert.fail(`Unexpected call ${valueType}`)
  }
  return reply(R.ReplyPayload.Ack, new R.AckT())
} }
const client = new RPCClient(wire)
live.client = client
const loop: (() => void)[] = []
page.requestAnimationFrame = (fn: () => void) => { loop.push(fn); return loop.length }
page.cancelAnimationFrame = () => {}
inner.listen(client)
let seq = 0n
function notify(type: R.NotificationPayload, value: R.NotificationT['value']) {
  const message = new R.NotificationT('sub', ++seq, type, value)
  const packet = frame(new R.MessageT(null, R.Payload.Notification, message))
  wire.onEvent!(packet)
  packet.fill(0) // The transport can reuse this buffer before the UI batch runs.
}
const flush = () => loop.shift()?.()
const first = '00000000-0000-4000-8000-000000000001'
const second = '00000000-0000-4000-8000-000000000002'
const unknown = '00000000-0000-4000-8000-000000000003'
const card = cardPath(['outer', 'a'])
const open = (session: string, w = 0, h = 0) =>
  notify(R.NotificationPayload.ViewOpened, new R.ViewOpenedT(session, card, 'same-view', 'html', w ? 1 : 0, w, h))
const send = (session: string, bytes: number[], creditId: bigint | null = null) =>
  notify(R.NotificationPayload.DisplayUpdated, new R.DisplayUpdatedT(new D.DisplayT(session, bytes, creditId)))

send(unknown, [7], 99n)
flush()
assert.equal(live.liveViews.size, 0, 'only ViewOpened creates a session')
open(first)
send(first, [0, 255, 128], 1n)
open(second, 640, 480)
send(second, [1, 0, 2], 2n)
send(first, [])
flush()
const a = live.liveViews.get(first)!
const b = live.liveViews.get(second)!
assert.deepEqual([b.card, b.name, b.mode, b.width, b.height], ['outer/a', 'same-view', 1, 640, 480])
assert.deepEqual(credits.map(c => c.creditId), [99n], 'credit waits for presentation')
const seen: [string, number[]][] = []
const capture = (session: string) => (message: ViewMessage) => {
  seen.push([session, [...message.bytes]])
  message.complete?.()
}
const off = a.listen(capture(first))
b.listen(capture(second))
assert.deepEqual(seen, [[first, [0, 255, 128]], [first, []], [second, [1, 0, 2]]])
assert.deepEqual(credits.map(c => [c.subscriptionId, c.session, c.bytes, c.creditId]), [
  ['sub', unknown, 1, 99n], ['sub', first, 3, 1n], ['sub', second, 3, 2n],
])
open(first) // A duplicate open cannot replace a running renderer.
flush()
assert.equal(live.liveViews.get(first), a)
off()
send(first, [99], 3n)
flush()
assert.equal(credits.at(-1)?.creditId, 3n, 'an unloaded renderer returns credit')

const recipient = 'outer/a'
const recipients: [string, boolean][] = []
a.attachRenderer({
  setRecipient: (path, registered) => { recipients.push([path, registered]) },
  sendInput: async () => {},
  snapshot: async () => new Uint8Array(),
  dispose() {},
})
const notifyRecipient = (session: string, registered: boolean) =>
  notify(R.NotificationPayload.ViewRecipientChanged, new R.ViewRecipientChangedT(session, recipient, registered))
notifyRecipient(first, true)
notifyRecipient(second, true)
notifyRecipient(first, false)
flush()
assert.deepEqual(recipients, [[recipient, true], [recipient, false]])
await client.sendView(first, recipient, Uint8Array.of(0, 255))
await client.sendView(second, '*', new Uint8Array())
assert.deepEqual(reverse.map(r => [r.session, r.to, r.bytes]), [
  [first, recipient, [0, 255]],
  [second, '*', []],
])
live.closeLiveView(first)
live.closeLiveView(first)
assert.deepEqual(closes.map(r => r.session), [first])
send(first, [42], 4n)
flush()
assert.equal(credits.at(-1)?.creditId, 4n, 'in-flight data returns credit after a close')
assert.equal(live.liveViews.get(second), b)

const controller = new AbortController()
assert.equal(await client.readCardView(second, controller.signal), new TextDecoder().decode(source))
assert.ok(reads.length > 1)
assert.deepEqual(reads.map(r => r.offset), reads.map((_, i) => BigInt(i * 2)))
assert.ok(reads.every(r => r.session === second))
controller.abort()
await assert.rejects(client.readCardView(second, controller.signal), { name: 'AbortError' })

open(unknown)
send(unknown, [8], 5n)
flush()
notify(R.NotificationPayload.ViewClosed, new R.ViewClosedT(unknown))
notify(R.NotificationPayload.ViewClosed, new R.ViewClosedT(unknown))
flush()
assert.equal(live.liveViews.get(unknown), undefined)
assert.equal(live.liveViews.get(second), b)
assert.deepEqual(closes.map(r => r.session), [first], 'a Card close needs no close request from the UI')
live.liveViews.reset()
assert.equal(live.liveViews.size, 0)
assert.equal(credits.at(-1)?.creditId, 5n)
console.log('live views: session presentation, FIFO, routes, credits, close and paged reads passed')
