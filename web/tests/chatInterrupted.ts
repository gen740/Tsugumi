// Reading an interrupted conversation never starts work, in either adapter.
import assert from 'node:assert/strict'
import { RemoteAgent, type CoreAgentHost } from '../server/coreAgent'

const operation = { kind: 'tool', id: 'call-7', name: 'project_save', arguments: '{"changes":[]}' }
const conversation = { id: 's', messages: [{ role: 'user', content: [{ type: 'text', text: 'hello' }] }], archive: [], tools: [], usage: { inputTokens: 0, outputTokens: 0 }, updatedAt: 0 }
const snapshot = { conversation, busy: false, resumable: true, continuable: true, interruptedOperation: operation, event_seq: 7n }
const calls: { method: string; fields: any }[] = []
const host = { user: { async call(method: string, fields: any) { calls.push({ method, fields }); return snapshot } } } as unknown as CoreAgentHost
const remote = await new RemoteAgent(host, 's').open()
await remote.refresh()
await remote.refresh()
assert.deepEqual(calls.map(c => c.method), ['ConversationCreateRequest', 'ConversationGetRequest', 'ConversationGetRequest'])
assert.equal(remote.busy, false)
assert.equal(remote.resumable, true)
assert.deepEqual(remote.interruptedOperation, operation)
await remote.action('Resume', () => {})
remote.close()
assert.equal(calls.filter(c => c.method === 'ConversationActionRequest').length, 1)
assert.equal(calls.find(c => c.method === 'ConversationActionRequest')?.fields.action, 'Resume')

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ChatController } = await import('../src/model/chat')
const previousFetch = globalThis.fetch
const requests: { url: string; body: any }[] = []
globalThis.fetch = (async (url: string, init: RequestInit) => {
  requests.push({ url, body: JSON.parse(String(init.body)) })
  return { ok: true, json: async () => ({ ...snapshot, event_seq: undefined, permission: null, provider: {} }) } as Response
}) as typeof fetch
try {
  const c = Object.create(ChatController.prototype) as any
  Object.assign(c, { items: [], notes: [], choices: [], offered: [], busy: false, turn: 0, available: true,
    transition: Promise.resolve(), session: { dir: '/p' }, currentId: '', folders: [], changed() {} })
  c.select('s')
  await new Promise(resolve => setTimeout(resolve, 0))
  c.failed = true // A lost stream's error must give way to the core interruption.
  await c.reread('s') // Reconnect uses this same read path.
  assert.deepEqual(requests.map(r => r.url), ['/api/chat/conversation', '/api/chat/conversation'])
  assert.ok(requests.every(r => !('resume' in r.body)))
  assert.equal(c.failed, false)
  assert.equal(c.busy, false)
  assert.equal(c.interrupted, true)
  assert.equal(c.stoppedAtLimit, false)
  assert.match(c.status, /project_save.*call-7.*changes/)
  c.resume()
  await new Promise(resolve => setTimeout(resolve, 0))
  assert.equal(requests.at(-1)?.url, '/api/chat/resume')
  assert.equal(c.interrupted, false)
  assert.equal(c.busy, true)
  // A person can instead submit a new message without a Resume request.
  c.select('s')
  await new Promise(resolve => setTimeout(resolve, 0))
  Object.assign(c, { references: [], skills: [], folderResources: new Map() })
  c.send('Please do this instead')
  await new Promise(resolve => setTimeout(resolve, 0))
  assert.equal(requests.at(-1)?.url, '/api/chat/send')
  assert.equal(requests.filter(r => r.url === '/api/chat/resume').length, 1)
} finally { globalThis.fetch = previousFetch }
console.log('Interrupted chat: opening/refresh/reconnect only read; Resume and new messages are explicit')
