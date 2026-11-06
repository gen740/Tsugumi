import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { Agent, compactionBoundary, fitToolResults, isContextOverflow } from '../server/agent'
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-compaction-'))
const behavior = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'tests/fixtures/agent-behavior.json'), 'utf8'))
const cases = behavior.compaction
for (const c of cases) {
  const cut = compactionBoundary(c.messages)
  assert.equal(cut, c.cut, c.name)
  assert.deepEqual([...c.messages.slice(0, cut), ...c.messages.slice(cut)], c.messages, c.name)
  if (cut < c.messages.length) assert.ok(!c.messages[cut].content.some((b: any) => b.type === 'tool_result'), c.name)
}
// The same cases as macOS/CoreTests (CompactionTests, ContextLimitTests).
for (const c of behavior.fitToolResults) {
  const messages = c.sizes.map((n: number, i: number) => ({ role: 'user', content: [{ type: 'tool_result', tool_use_id: `t${i}`, content: 'r'.repeat(n), is_error: false }] }))
  const fitted = fitToolResults(messages, c.room)
  assert.deepEqual(fitted.map((m: any) => m.content[0].content.length), c.expect, c.name)
  assert.deepEqual(fitToolResults(fitted, c.room), fitted, c.name)
}
for (const c of behavior.contextOverflow) assert.equal(isContextOverflow(c.text), c.overflow, c.text)
const originalFetch = globalThis.fetch
try {
  let input = ''
  globalThis.fetch = async (_url, options) => {
    input = JSON.parse(String(options!.body)).messages[0].content[0].text
    return new Response('data: {"type":"content_block_delta","delta":{"text":"combined summary"}}\n\ndata: {"type":"message_stop"}\n\n')
  }
  const a = Object.create(Agent.prototype) as any
  a.storage = path.join(dir, 'chat.json')
  a.provider = { api: 'anthropicMessages', baseURL: 'http://fixture.invalid', auth: 'none', contextLimit: 10_000 }
  a.conversation = { messages: structuredClone(cases.find((c: any) => c.name === 'complete_pairs').messages),
    archive: [], summary: 'EARLIER', usage: { inputTokens: 100 } }
  const original = structuredClone(a.conversation.messages)
  assert.equal(compactionBoundary(original), 3)
  await a.compactIfNeeded(() => {}, new AbortController().signal, true)
  assert.ok(input.includes('EARLIER') && input.includes('RESULT_1'))
  assert.ok(JSON.stringify(a.wireMessages()).includes('RESULT_3'))
  assert.equal(a.conversation.summary, 'combined summary')
  assert.deepEqual([...a.conversation.archive, ...a.conversation.messages], original)
  assert.equal(compactionBoundary([...original, { role: 'assistant', content: [{ type: 'text', text: 'done' }] }]), 3)
} finally { globalThis.fetch = originalFetch; fs.rmSync(dir, { recursive: true, force: true }) }
console.log('compaction: passed')
