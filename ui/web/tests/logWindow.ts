import assert from 'node:assert/strict'
import { logLineNumber, LogHeights, LogReadFrames, LogSlice, LogWindow } from '../src/model/logWindow'

function slice(offset: number, text: string, line: number, startsLine: boolean, size?: number): LogSlice {
  const end = offset + new TextEncoder().encode(text).length
  return { text, offset, end, size: size ?? end, line, startsLine }
}

const terminal = new LogWindow('terminal')
const previous = 'first\n日本 読み込み'
const boundary = new TextEncoder().encode(previous).length
terminal.apply(terminal.request('tail')!, slice(boundary, '\r完了\nnext\n', 2, false))
terminal.follow(false)
const before = new LogHeights(terminal.blocks, new Map(), 20)
const anchor = before.anchor(27)!
assert.equal(anchor.line, 3)
terminal.apply(terminal.request('earlier')!, slice(0, previous, 1, true, terminal.end))
const after = new LogHeights(terminal.blocks, new Map([[terminal.blocks[0], 40]]), 20)
assert.equal(after.position(anchor), 67)
assert.deepEqual(terminal.blocks.flatMap((b) => b.lines.map((text, i) => ({ text, number: logLineNumber(b, i) }))), [
  { number: 1, text: 'first' }, { number: 2, text: '完了 読み込み' }, { number: 3, text: 'next' },
])
assert.equal(terminal.request('earlier'), undefined)

const journal = new LogWindow('journal')
const partial = '{"phase":"save","event":"st'
journal.apply(journal.request('tail')!, slice(partial.length, 'ored","message":"ok"}\n', 7, false))
assert.equal(journal.blocks.length, 0)
journal.follow(false)
journal.apply(journal.request('earlier')!, slice(0, partial, 7, true, journal.end))
assert.deepEqual(journal.blocks.flatMap((b) => b.lines), ['save · stored: ok'])

const paging = new LogWindow('terminal')
const payload = 'x'.repeat((1 << 20) - 1) + '\n'
const length = payload.length
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
assert.equal(paging.end, 3 * length + 1)
assert.equal(paging.waiting, false)

const longLine = new LogWindow('terminal')
const a = 'a'.repeat(256 * 1024), b = 'b'.repeat(256 * 1024)
longLine.apply(longLine.request('tail')!, slice(a.length, b, 1, false))
const oldAnchor = new LogHeights(longLine.blocks, new Map(), 20).anchor(13)!
longLine.follow(false)
longLine.apply(longLine.request('earlier')!, slice(0, a, 1, true, a.length + b.length))
const measured = new Map([[longLine.blocks[0], 900]])
assert.equal(new LogHeights(longLine.blocks, measured, 20).position(oldAnchor), 913)
assert.deepEqual(longLine.blocks.map((block) => block.first), [1, 1])

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
