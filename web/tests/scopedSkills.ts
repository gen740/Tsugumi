import assert from 'node:assert/strict'
import { helperScopeLabel } from '../src/shared/agentShared'

const page = globalThis as Record<string, any>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
const originalFetch = globalThis.fetch
let send: any
const flush = async () => { for (let i = 0; i < 40; i++) await Promise.resolve() }
globalThis.fetch = (async (url, init) => {
  let value: unknown = {}
  if (url === '/api/chat/sessions') value = [{ id: 'scope-chat', title: 'Chat', updatedAt: 0, messages: 0 }]
  if (url === '/api/skills') value = [{ name: 'review', path: 'builtin:review', scoped: true, scope: ['read'] }]
  if (url === '/api/chat/conversation') value = { conversation: { id: 'scope-chat', rows: [], messages: [], archive: [], helpers: [], usage: {} } }
  if (url === '/api/chat/send') send = JSON.parse(String(init!.body))
  return { ok: true, json: async () => value } as Response
}) as typeof fetch
const { ChatController } = await import('../src/model/chat')
const chat = new ChatController({ dir: '/scope-test', displayPath: (p: string[]) => p.join('/') } as never, { status: { chatAvailable: true } } as never)
try {
  await flush()
  chat.send('$review', ['outer/a', 'b'], 'review')
  await flush()
  assert.deepEqual(send.selectedCards, ['outer/a', 'b'])
  assert.deepEqual(send.skillNames, ['review'])
  assert.equal(send.scopedSkill, 'review')
  assert.equal(send.text, '$review', 'The client never inserts a Skill body or manufactures scope sets')
  const task = { name: 'review', skill_path: 'builtin:review', read_cards: ['outer/a', 'b'], write_cards: [], run_cards: [] }
  const label = helperScopeLabel('project_access helper_scope\n' + JSON.stringify({ task }))
  assert.match(label, /read: outer\/a, b/)
  assert.match(label, /write: —/)
  assert.match(label, /run: —/)
  const internal = chat as any
  internal.load({ rows: [{ id: 'turn:0', kind: 'helper', name: 'review', state: 'completed', text: 'Read only: finding', instruction: 'Review the selected Cards', scopeJa: '読む: outer/a, b, 変更: なし, 実行: なし', scopeEn: 'Read: outer/a, b, Change: none, Run: none', children: [] }] })
  assert.equal(chat.items[0].role, 'helper')
  assert.equal(chat.items[0].helper, 'review')
  assert.match(chat.items[0].text, /Read only: finding/)
  assert.match(chat.items[0].presentation!.scopeEn, /Run: none/)
  assert.equal(chat.items[0].presentation!.instruction, 'Review the selected Cards')
} finally { chat.close(); globalThis.fetch = originalFetch }
console.log('scoped Skills: exact user selection, one scope description and durable helper trace passed')
