import assert from 'node:assert/strict'
import { ModuleThumbnailQueue, moduleThumbnailKey, settledThumbnailSize, ThumbnailRequest } from '../src/model/moduleThumbnails'
const sleep = (ms = 5) => new Promise(r => setTimeout(r, ms))
let calls: string[] = [], active = 0, peak = 0, closed = 0
let finish: (image: string) => void = () => {}
const renderer = {
  async draw(request: ThumbnailRequest) {
    calls.push(request.key); active++; peak = Math.max(active, peak)
    const result = await new Promise<string>(resolve => { finish = resolve })
    active--; return { image: result, pixelBytes: 64 }
  },
  close() { closed++ },
}
const queue = new ModuleThumbnailQueue(renderer, 1000, 5)
const req = (key: string): ThumbnailRequest => ({ key, name: 'plotly', json: key, width: 2, height: 2 })
const a = new AbortController(), b = new AbortController(), c = new AbortController()
const first = queue.request(req('first'), a.signal)
const same = queue.request(req('first'), b.signal)
const removed = queue.request(req('removed'), c.signal)
c.abort()
assert.equal(await removed, undefined)
await sleep(); assert.deepEqual(calls, ['first'])
a.abort(); assert.equal(await first, undefined)
finish('png-first'); assert.equal(await same, 'png-first')
assert.equal(await queue.request(req('first'), new AbortController().signal), 'png-first')
assert.equal(calls.length, 1, 'same content is rendered once')
const second = queue.request(req('second'), new AbortController().signal)
const third = queue.request(req('third'), new AbortController().signal)
await sleep(); finish('png-second'); await second
await sleep(); finish('png-third'); await third
assert.equal(peak, 1)
await sleep(15); assert.equal(closed, 1, 'idle runtime is released')
assert.notEqual(await moduleThumbnailKey('plotly', '{"data":[1]}', 100, 80), await moduleThumbnailKey('plotly', '{"data":[2]}', 100, 80))
assert.notEqual(await moduleThumbnailKey('plotly', '{}', 100, 80), await moduleThumbnailKey('vega-lite', '{}', 100, 80))
assert.notEqual(await moduleThumbnailKey('plotly', '{}', 100, 80), await moduleThumbnailKey('plotly', '{}', 120, 80))
console.log('module thumbnails: shared work, cancellation, content identity and idle cleanup passed')

{
  const calls: string[] = []
  let closed = 0
  const bounded = new ModuleThumbnailQueue({
    async draw(request) { calls.push(request.key); if (request.key === 'bad') throw Error('render failed'); return { image: 'x'.repeat(100), pixelBytes: 64 } },
    close() { closed++ },
  }, 300, 5)
  const signal = new AbortController().signal
  await bounded.request(req('one'), signal)
  await bounded.request(req('two'), signal)
  await bounded.request(req('one'), signal)
  assert.deepEqual(calls, ['one', 'two', 'one'], 'budget evicts old encoded images and decoded pixel cost')
  await assert.rejects(bounded.request(req('bad'), signal), /render failed/)
  assert.equal(closed, 1, 'failed renderer is disposed before the next request')
  await bounded.request(req('recovered'), signal)
  await sleep(15)
}

let size = settledThumbnailSize(264, 200, 1, { width: 0, height: 0 })
for (const zoom of [0.83, 0.31, 1.21, 2]) {
  assert.equal(settledThumbnailSize(Math.round(264 * zoom), Math.round(200 * zoom), zoom, size), size)
}
assert.notDeepEqual(settledThumbnailSize(500, 300, 1, size), size, 'resizing creates a new preview')
