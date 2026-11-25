// Reading an interrupted conversation never starts work.
import assert from 'node:assert/strict'
import { L } from '../src/i18n'

const operation = { kind: 'tool', id: 'call-7', name: 'project_save', arguments: '{"changes":[]}' }
const conversation = { id: 's', rows: [], messages: [{ role: 'user', content: [{ type: 'text', text: 'hello' }] }], archive: [], tools: [], usage: { inputTokens: 0, outputTokens: 0 }, updatedAt: 0 }
const snapshot = { conversation, busy: false, resumable: true, continuable: true, interruptedOperation: operation, event_seq: 7n }
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
  return { ok: true, json: async () => ({ ...snapshot, permissions: [], provider: {} }) } as Response
}) as typeof fetch
try {
  const c = Object.create(ChatController.prototype) as any
  Object.assign(c, { items: [], notes: [], choices: [], offered: [], busy: false, turn: 0, reload: 0, contentRevision: 0, available: true,
    transition: Promise.resolve(), session: { dir: '/p' }, currentId: '', folders: [], changed() {} })
  let reading: Promise<unknown>
  const reread = c.reread.bind(c)
  c.reread = (id: string) => (reading = reread(id))
  c.select('s')
  await reading!
  c.failed = true // A lost stream's error must give way to the core interruption.
  await c.reread('s') // Reconnect uses this same read path.
  assert.deepEqual(requests.map(r => r.url), ['/api/chat/conversation', '/api/chat/conversation'])
  assert.ok(requests.every(r => !('resume' in r.body)))
  assert.equal(c.failed, false)
  assert.equal(c.busy, false)
  assert.equal(c.interrupted, true)
  assert.equal(c.canContinue, false)
  assert.match(c.status, /project_save.*call-7.*changes/)
  c.resume()
  await c.transition
  assert.equal(requests.filter(r => r.url === '/api/chat/resume').length, 1)
  assert.equal(c.interrupted, false)
  assert.equal(c.busy, true)
  // A person can instead submit a new message without a Resume request.
  c.select('s')
  await reading!
  Object.assign(c, { references: [], skills: [], folderResources: new Map() })
  c.send('Please do this instead')
  await c.transition
  assert.equal(requests.filter(r => r.url === '/api/chat/send').length, 1)
  assert.equal(requests.filter(r => r.url === '/api/chat/resume').length, 1)
} finally { globalThis.fetch = previousFetch }
console.log('Interrupted chat: opening/refresh/reconnect only read; Resume and new messages are explicit')

const lockMessage = L('{0}（PID {1}）がこのプロジェクトの会話を開いています。そのプロセスを終了してから再読込してください。', 'Tsugumi Dev', 2345)
let locked = true
globalThis.fetch = (async (url: string) => {
  if (locked) return { ok: false, json: async () => ({ code: 'busy', error: lockMessage }) } as Response
  if (url === '/api/chat/sessions')
    return { ok: true, json: async () => [{ id: 's' }] } as Response
  assert.equal(url, '/api/chat/conversation')
  return { ok: true, json: async () => ({ ...snapshot, permissions: [], provider: {} }) } as Response
}) as typeof fetch
try {
  const c = Object.create(ChatController.prototype) as any
  Object.assign(c, { currentId: '', turn: 0, reload: 0, available: true,
    session: { dir: '/p' }, folders: [], changed() {}, reloadSkills() {} })
  const init = c.init.bind(c)
  let loading = init()
  c.init = () => (loading = init())
  let reading: Promise<unknown>
  const reread = c.reread.bind(c)
  c.reread = (id: string) => (reading = reread(id))
  await loading
  assert.equal(c.loadError, L('エラー: {0}', lockMessage))
  assert.equal(c.currentId, '')
  locked = false
  c.retryLoad()
  await loading
  await reading!
  assert.equal(c.currentId, 's')
  assert.equal(c.ready, true)
  assert.equal(c.loadError, undefined)
  locked = true
  c.retryLoad()
  await reading!
  assert.equal(c.ready, false)
  assert.equal(c.loadError, L('エラー: {0}', lockMessage))
  locked = false
  c.retryLoad()
  await reading!
  assert.equal(c.ready, true)
} finally { globalThis.fetch = previousFetch }
console.log('Conversation lock: the chat shows the holder and Reload retries the conversation list')
