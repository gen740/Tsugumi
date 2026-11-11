import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { Agent, messagesBody, resultBlock, responsesInput, splitImages } from '../server/agent'

// The same cases as the Swift Agent (tests/fixtures/agent-behavior.json).
const cases = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'tests/fixtures/agent-behavior.json'), 'utf8')).toolImages
for (const c of cases) {
  const { text, images } = splitImages(c.result)
  if (c.text) assert.deepEqual(JSON.parse(text), c.text, c.name)
  else assert.equal(text, c.result, c.name)
  assert.deepEqual(images, c.images, c.name)
}

const png = cases[0].images[0].data as string
const padded = (n: number) => {
  const b = Buffer.from(png, 'base64')
  return Buffer.concat([b.subarray(0, 33), Buffer.alloc(n), b.subarray(33)]).toString('base64')
}
const large = splitImages(JSON.stringify({ data: { base64: padded(8 << 20) } }))
assert.ok(!large.images.length && large.text.includes('too large to show') && large.text.length < 100)

// A long image no longer fills the text, and the block still carries it.
const long = padded(200_000)
const block: any = resultBlock('t', JSON.stringify({ data: { base64: long } }), false)
assert.equal(block.images[0].data, long)
assert.ok(!block.content.includes('cut:'))

// Both providers get the image.
const shown = { type: 'tool_result' as const, tool_use_id: 't', content: 'text', is_error: false, images: [{ media_type: 'image/png', data: 'AAAA' }] }
const provider: any = { api: 'anthropicMessages', baseURL: 'http://fixture.invalid', auth: 'none', contextLimit: 1000, model: 'm', maxTokens: 10 }
const wired: any = messagesBody(provider, 'system', [{ role: 'user', content: [shown] }], [])
const parts = wired.messages[0].content[0].content
assert.equal(parts[0].text, 'text')
assert.deepEqual(parts[1].source, { type: 'base64', media_type: 'image/png', data: 'AAAA' })
const plain: any = messagesBody(provider, 'system', [{ role: 'user', content: [{ ...shown, images: undefined }] }], [])
assert.equal(plain.messages[0].content[0].content, 'text')
const input: any = responsesInput([{ role: 'user', content: [shown] }])
assert.equal(input[0].output[0].text, 'text')
assert.equal(input[0].output[1].image_url, 'data:image/png;base64,AAAA')

// A provider that refuses an image would refuse every later request of the
// conversation too: the images go, and the model is told.
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-images-'))
const originalFetch = globalThis.fetch
try {
  const bodies: any[] = []
  globalThis.fetch = async (_url, options) => {
    const body = JSON.parse(String(options!.body))
    bodies.push(body)
    if (JSON.stringify(body.messages).includes('"image"')) return new Response('{"error":{"message":"Could not process image"}}', { status: 400 })
    return new Response('data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"ok"}}\n\ndata: {"type":"message_stop"}\n\n')
  }
  const a = Object.create(Agent.prototype) as any
  a.storage = path.join(dir, 'chat.json')
  a.provider = provider
  Object.defineProperties(a, { system: { value: 'system' }, tools: { value: [] } })
  a.conversation = { id: 'c', updatedAt: 0, archive: [], tools: [], usage: { inputTokens: 0, outputTokens: 0 }, messages: [{ role: 'user', content: [shown] }] }
  const events: any[] = []
  await a.modelTurn((e: any) => events.push(e), new AbortController().signal)
  assert.equal(bodies.length, 2)
  assert.ok(events.some((e) => e.kind === 'retrying' && e.message.includes('image')))
  const left = a.conversation.messages[0].content[0]
  assert.equal(left.images, undefined)
  assert.ok(left.content.includes('did not accept'))
} finally {
  globalThis.fetch = originalFetch
  fs.rmSync(dir, { recursive: true, force: true })
}
console.log('tool images: passed')
