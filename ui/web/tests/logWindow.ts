import assert from 'node:assert/strict'
import { Builder, ByteBuffer } from 'flatbuffers'
import { R, D, RPCClient } from '../src/wire/rpc'
import { decodeLog, readLogFile, logLineNumber, LogHeights, LogRead, LogReadFrames, LogSlice, LogWindow } from '../src/model/logWindow'

function slice(offset: number, text: string, line: number, startsLine: boolean, size?: number, terminal = true): LogSlice {
  const end = offset + (terminal ? Array.from(text).length * 4 : new TextEncoder().encode(text).length)
  return { text, offset, end, size: size ?? end, line, startsLine }
}

const terminal = new LogWindow('terminal')
const previous = 'first\n完了 読み込み'
const boundary = Array.from(previous).length * 4
terminal.apply(terminal.request('tail')!, slice(boundary, '\nnext\n', 2, false))
terminal.follow(false)
const before = new LogHeights(terminal.blocks, new Map(), 20)
const anchor = before.anchor(7)!
assert.equal(anchor.line, 3)
terminal.apply(terminal.request('earlier')!, slice(0, previous, 1, true, terminal.end))
const after = new LogHeights(terminal.blocks, new Map([[terminal.blocks[0], 40]]), 20)
assert.equal(after.position(anchor), 67)
assert.deepEqual(terminal.blocks.flatMap((b) => b.lines.map((text, i) => ({ text, number: logLineNumber(b, i) }))), [
  { number: 1, text: 'first' }, { number: 2, text: '完了 読み込み' }, { number: 3, text: 'next' },
])
assert.equal(terminal.request('earlier'), undefined)

const journal = new LogWindow('journal')
const byteCount = (text: string) => new TextEncoder().encode(text).length
const partial = 'save · st'
journal.apply(journal.request('tail')!, slice(byteCount(partial), 'ored: ok\n', 7, false, undefined, false))
assert.equal(journal.blocks.length, 0)
journal.follow(false)
journal.apply(journal.request('earlier')!, slice(0, partial, 7, true, journal.end, false))
assert.deepEqual(journal.blocks.flatMap((b) => b.lines), ['save · stored: ok'])
assert.equal(journal.blocks[0].first, 7)

const paging = new LogWindow('terminal')
const payload = 'x'.repeat((1 << 18) - 1) + '\n'
const length = payload.length * 4
paging.apply(paging.request('tail')!, slice(2 * length, payload, 3, true))
paging.follow(false)
paging.apply(paging.request('earlier')!, slice(length, payload, 2, true, 3 * length))
paging.apply(paging.request('earlier')!, slice(0, payload, 1, true, 3 * length))
assert.deepEqual([paging.start, paging.end], [0, 2 * length])
paging.apply(paging.request('later')!, slice(2 * length, payload, 3, true))
assert.deepEqual([paging.start, paging.end], [length, 3 * length])
assert.equal(paging.waiting, false)
paging.apply(paging.request('status')!, slice(3 * length, 'a', 4, true))
assert.equal(paging.end, 3 * length)
assert.equal(paging.waiting, true)
paging.follow(true)
paging.apply(paging.request('status')!, slice(3 * length, 'a', 4, true))
assert.equal(paging.end, 3 * length + 4)
assert.equal(paging.waiting, false)

const longLine = new LogWindow('terminal')
const a = 'a'.repeat(256 * 1024), b = 'b'.repeat(256 * 1024)
longLine.apply(longLine.request('tail')!, slice(a.length * 4, b, 1, false))
const oldAnchor = new LogHeights(longLine.blocks, new Map(), 20).anchor(13)!
longLine.follow(false)
longLine.apply(longLine.request('earlier')!, slice(0, a, 1, true, (a.length + b.length) * 4))
const earlierRows = longLine.blocks.filter((block) => block.lines[0].startsWith('a'))
const measured = new Map(earlierRows.map((block) => [block, 900]))
assert.equal(new LogHeights(longLine.blocks, measured, 20).position(oldAnchor), 900 * earlierRows.length + 13)
assert.ok(longLine.blocks.every((block) => block.first === 1))

// Crossing the same terminal cells with a differently sized page preserves
// the fragment containing an absolute position.
const overlap = new LogWindow('terminal')
overlap.apply(overlap.request('tail')!, slice((a.length + 3000) * 4, b.slice(3000), 1, false))
const stable = overlap.blocks[1]
assert.deepEqual(stable, longLine.blocks.find((block) => block.key === stable.key))

// Exercise the request offsets as a user traverses a 128 MiB stream with
// no line delimiters. Holding the stream itself in the test is unnecessary.
const large = new LogWindow('terminal')
const size = 4 * (128 << 20)
const bytes = 'z'.repeat(1 << 18)
function load(read: LogRead) {
  const offset = read.kind === 'tail' ? size - read.limit : read.kind === 'earlier' ? Math.max(0, read.offset - read.limit) : read.offset
  const end = read.kind === 'earlier' ? read.offset : Math.min(size, offset + read.limit)
  large.apply(read, { text: bytes.slice(0, (end - offset) / 4), offset, end, size, line: 1, startsLine: offset === 0 })
  assert.ok(large.end - large.start <= 2 << 20)
}
load(large.request('tail')!)
large.follow(false)
for (let request = large.request('earlier'); request; request = large.request('earlier')) load(request)
assert.equal(large.start, 0)
for (let request = large.request('later'); request; request = large.request('later')) load(request)
assert.equal(large.end, size)

const callbacks: (() => void)[] = []
let reads = 0, applied = 0
const gate = new LogReadFrames((fn) => callbacks.push(fn), () => {}, async () => {
  ++reads
  return () => { ++applied }
})
for (let i = 0; i < 100; ++i) gate.wake()
assert.equal(callbacks.length, 1)
callbacks.shift()!()
assert.equal(reads, 1)
for (let i = 0; i < 100; ++i) gate.wake()
assert.equal(callbacks.length, 0)
await Promise.resolve()
assert.equal(callbacks.length, 1)
callbacks.shift()!()
assert.deepEqual([applied, reads], [1, 2])
await Promise.resolve()
callbacks.shift()!()
assert.equal(applied, 2)
gate.close()

const encoded = [
  new TextEncoder().encode('x'.repeat((1 << 20) - 3) + '雪'),
  new TextEncoder().encode('\ufeff\rB\n'),
]
const fileSize = encoded.reduce((sum, piece) => sum + piece.length, 0)
const rpc = new RPCClient({
  close() {},
  async call(frame) {
    const message = R.Message.getRootAsMessage(new ByteBuffer(frame)).unpack()
    const request = (message.payload as R.RequestT).value as R.LogRangeRequestT
    const offset = request.offset ?? 0n
    const piece = encoded[offset === 0n ? 0 : 1]
    const reply = new R.LogSliceT(piece, offset, offset + BigInt(piece.length), BigInt(fileSize))
    const builder = new Builder()
    builder.finish(new R.MessageT(message.requestId, R.Payload.Reply,
      new R.ReplyT(R.ReplyPayload.LogSlice, reply)).pack(builder), 'TSG1')
    return builder.asUint8Array()
  },
})
const file = await readLogFile(fileSize, (offset) => rpc.logRange('run', null, null, D.LogStream.build, offset))
assert.deepEqual(Buffer.from(await file.arrayBuffer()), Buffer.concat(encoded))
assert.ok(decodeLog((await rpc.logRange('run', null, null, D.LogStream.build, BigInt(encoded[0].length))).text).startsWith('\ufeff'))

const sparse = new LogWindow('journal')
const record = 'save · stored: anchor\n'
const ignored = 'build · done: ' + 'x'.repeat((1 << 20) - 64) + '\n'
const rawPage = (offset: number, line: number, raw: string, size: number) => slice(offset, raw, line, true, size, false)
const available = 4 * (1 << 20)
const anchorPage = rawPage(available - byteCount(record), 4, record, available)
sparse.apply(sparse.request('tail')!, anchorPage)
sparse.follow(false)
sparse.visible = { first: 4, last: 4, fragments: new Set() }
for (let line = 3; line >= 1; line--) {
  sparse.apply(sparse.request('earlier')!, rawPage(sparse.start - byteCount(ignored), line, ignored, available))
}
assert.ok(sparse.end < anchorPage.offset)
assert.deepEqual(sparse.blocks.flatMap((b) => b.lines), ['save · stored: anchor'])
