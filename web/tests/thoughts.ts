import { mockHostSettings } from './hostSettingsFixture'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { Thoughts, type ThoughtReply } from '../src/model/thoughts'
import { chatRoutes } from '../server/chat'
import { CoreAgentHost, RemoteAgent } from '../server/coreAgent'
import EN from '../src/i18n/en.json'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { guide } = await import('../src/model/guide')

const saved = new Map<string, string>()
const storage = { getItem: (k: string) => saved.get(k) ?? null, setItem: (k: string, v: string) => void saved.set(k, v) }
let context = { identity: 'first:turn-1', active: true }
let shown: string | undefined, calls = 0
let resolve!: (r: ThoughtReply) => void
let reject!: (e: Error) => void
const thoughts = () => new Thoughts(() => context, () => {
  calls++
  return new Promise((yes, no) => { resolve = yes; reject = no })
}, (text) => { shown = text }, () => {}, storage)
const reply = (text: string): ThoughtReply => ({ text, usage: { inputTokens: 13, outputTokens: 5 } })

// Neither opening the chat nor cancelling the consent spends tokens. Consent
// and the on/off choice survive reopening; off/on does not ask a second time.
const t = thoughts()
await t.refresh()
assert.equal(calls, 0)
assert.equal(t.toggle(), false)
assert.equal(t.enabled, false)
assert.equal(saved.size, 0)
await t.refresh()
assert.equal(calls, 0)
t.accept()
assert.equal(t.enabled, true)
assert.equal(thoughts().enabled, true)

// One window never overlaps its own requests. Tokens belong to this feature,
// including replies dropped after Stop, switching conversations or closing.
let pending = t.refresh()
await t.refresh()
assert.equal(calls, 1)
resolve(reply('「帯域を確認しています」\nextra'))
await pending
assert.equal(shown, '帯域を確認しています')
assert.deepEqual(t.usage, { inputTokens: 13, outputTokens: 5 })
assert.match(t.label, /入力 13、出力 5/)
pending = t.refresh()
resolve({ text: '', usage: { inputTokens: 0, outputTokens: 0 } })  // Core skipped unchanged work.
await pending
assert.equal(shown, '帯域を確認しています')
pending = t.refresh()
context = { ...context, active: false }
resolve(reply('late after Stop'))
await pending
assert.equal(shown, '帯域を確認しています')
assert.equal(t.usage.inputTokens, 26)
await t.refresh()
assert.equal(calls, 3)
context = { identity: 'first:turn-2', active: true }
pending = t.refresh()
context = { identity: 'second:turn-1', active: true }
resolve(reply('wrong conversation'))
await pending
assert.equal(shown, '帯域を確認しています')
pending = t.refresh()
assert.equal(t.toggle(), true)
assert.equal(shown, undefined)
assert.equal(t.enabled, false)
assert.equal(thoughts().enabled, false)
assert.equal(t.toggle(), true)
resolve(reply('from before off/on'))
await pending
assert.equal(shown, undefined)
pending = t.refresh()
reject(new Error('model unavailable'))
await pending
assert.equal(t.lastError, 'model unavailable')
assert.equal(shown, undefined)
pending = t.refresh()
t.close()
resolve(reply('closed project'))
await pending
assert.equal(shown, undefined)
assert.equal(t.usage.outputTokens, 25)

// A disabled storage still permits explicit consent for the current window.
const denied = new Thoughts(() => context, async () => reply('ok'), () => {}, () => {}, {
  getItem() { throw new Error('denied') }, setItem() { throw new Error('denied') },
})
assert.equal(denied.toggle(), false)
denied.accept()
assert.equal(denied.enabled, true)

// Canvas presentation: eight seconds, a newer scene, Stop and leaving clear
// the summary, so the existing plain work line can be shown again.
const c = { busy: true, thoughts: t, session: guide.contextSession } as any
const clock = Date.now
let now = 1000
Date.now = () => now
const wasAvailable = guide.available
try {
  guide.available = true
  guide.chatThought(c, '帯域を確認しています')
  assert.equal(guide.thoughtLine(), '帯域を確認しています')
  guide.chatThought({ ...c, session: {} } as any, 'another project')
  assert.equal(guide.thoughtLine(), undefined)
  guide.chatThought(c, '帯域を確認しています')
  now += 7999
  assert.equal(guide.thoughtLine(), '帯域を確認しています')
  now++
  assert.equal(guide.thoughtLine(), undefined)
  guide.chatThought(c, 'summary')
  guide.chatWork('project_file_read', '{"path":"definitions/rgb/main.py"}')
  assert.equal(guide.thoughtLine(), undefined)
  guide.chatThought(c, 'summary')
  c.busy = false
  assert.equal(guide.thoughtLine(), undefined)
  c.busy = true
  guide.chatWorkEnded()
  assert.equal(guide.thoughtLine(), undefined)
  guide.chatThought(c, 'summary')
  guide.chatClosed(c)
  assert.equal(guide.thoughtLine(), undefined)
} finally {
  Date.now = clock
  guide.available = wasAvailable
}

// The adapter sends the existing FlatBuffers request, including refreshed
// provider references. No provider calls or sockets are opened in these tests.
const host = new CoreAgentHost('/nonexistent', '/nonexistent', {} as never, 'ja')
let forwarded: any
host.user.call = async (method, fields) => {
  assert.equal(method, 'ConversationSummarizeRequest')
  forwarded = fields
  return reply('working') as any
}
assert.deepEqual(await host.summarize('conversation', {
  name: 'ChatGPT', model: 'small', api: 'openAIResponses', auth: 'chatgpt',
}), reply('working'))
assert.equal(forwarded.conversation_id, 'conversation')
assert.equal(forwarded.provider.provider_id, 'chatgpt')
assert.equal(forwarded.provider.credential, undefined)
assert.equal(forwarded.provider.credential_id, undefined)

// Page -> host route -> the same core adapter. The route chooses the speech
// connection already used by the guide and never returns provider credentials.
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-thoughts-'))
const original = { open: RemoteAgent.prototype.open, observe: RemoteAgent.prototype.observe, refresh: RemoteAgent.prototype.refresh }
const key = process.env.ANTHROPIC_API_KEY
process.env.ANTHROPIC_API_KEY = 'test-key'
try {
  RemoteAgent.prototype.open = async function () {
    this.host.user.call = async (method, fields) => {
      if (method === 'AccessListRequest') return { requests: [] } as any
      if (method === 'ProviderSetRequest') {
        assert.equal(fields.ephemeral, true)
        assert.equal(fields.provider.api_key, 'test-key')
        return { configured: true } as any
      }
      assert.equal(method, 'ConversationSummarizeRequest')
      forwarded = fields
      return reply('reading') as any
    }
    return this
  }
  RemoteAgent.prototype.observe = () => {}
  RemoteAgent.prototype.refresh = async () => {}
  const restoreSettings = mockHostSettings()
    const routes = chatRoutes({ support: root, here: root, envs: {}, projectDir: (p: unknown) => String(p),
  } as any)
  const result = await routes['POST /api/chat/thoughts']({ path: root, session: 'web-thoughts' }, new URL('http://localhost'))
  restoreSettings()
  assert.deepEqual(result, reply('reading'))
  assert.equal(forwarded.conversation_id, 'web-thoughts')
  assert.equal(forwarded.provider.model, 'claude-haiku-5-5')
  assert.equal(JSON.stringify(result).includes('test-key'), false)
} finally {
  Object.assign(RemoteAgent.prototype, original)
  if (key === undefined) delete process.env.ANTHROPIC_API_KEY
  else process.env.ANTHROPIC_API_KEY = key
  fs.rmSync(root, { recursive: true, force: true })
}

assert.equal(EN['思考の過程を描きますか？'], "Draw the Agent's thoughts?")
assert.match(EN['思考の過程を描く: オン（使った token: 入力 {0}、出力 {1}）'], /input \{0\}, output \{1\}/)
console.log('draw thoughts behavior: passed')
