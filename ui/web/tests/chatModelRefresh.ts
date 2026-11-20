import assert from 'node:assert/strict'
import { Observable } from '../src/model/observable'
import { LiveCodePlayer } from '../src/model/liveCode'

// Drive presentation and stream events directly: no sockets or timer waits.
const page = globalThis as Record<string, any>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
let stream: { onmessage?: (event: { data: string }) => void }
page.EventSource = class { constructor() { stream = this } }
const originalFetch = globalThis.fetch, originalInterval = globalThis.setInterval
let intervals = 0, reads = 0
let selected = 'gpt-6.1-sol', accepted: string | undefined
const provider = () => ({ name: 'ChatGPT', model: accepted ?? selected, api: 'openAIResponses', plan: true, remote: true, modelNames: {} })
const settings = () => ({ provider: 'chatgpt', chatGPTModel: selected, chatProvider: provider(), revision: 'host:1' })
const flush = async () => { for (let i = 0; i < 40; i++) await Promise.resolve() }
globalThis.setInterval = ((...args: Parameters<typeof setInterval>) => { intervals++; return originalInterval(...args) }) as typeof setInterval
globalThis.fetch = (async (url, init) => {
  let value: unknown = {}
  if (url === '/api/chat/sessions') value = [{ id: 's', title: 'Chat', updatedAt: 0, messages: 0 }]
  if (url === '/api/guide') value = { available: true, busy: false, conversation: null }
  if (url === '/api/skills') value = []
  if (url === '/api/chat/conversation') {
    reads++
    value = { provider: provider(), busy: !!accepted, conversation: { id: 's', rows: [], messages: [], archive: [], usage: {} } }
  }
  if (url === '/api/settings') {
    if (init?.body) selected = JSON.parse(String(init.body)).chatGPTModel ?? selected
    value = settings()
  }
  return { ok: true, json: async () => value } as Response
}) as typeof fetch
const { ChatController } = await import('../src/model/chat')
const { api } = await import('../src/api')
const session = Object.assign(new Observable(), { dir: '/model-refresh', currentRunId: undefined as string | undefined, runActive: false })
const chat = new ChatController(session as never, { status: { chatAvailable: true } } as never)
try {
  await flush()
  assert.equal(chat.provider.model, selected)
  assert.equal(intervals, 0, 'an open chat installs no model polling timer')
  const visible = Object.assign(new EventTarget(), { visibilityState: 'visible' })
  const viewport = new EventTarget()
  selected = 'gpt-6.1-terra'
  const leave = chat.observePresentation(visible as never, viewport as never)
  await flush()
  assert.equal(chat.provider.model, selected, 'panel appearance')
  for (const [target, event] of [[visible, 'visibilitychange'], [viewport, 'focus']] as const) {
    selected = selected === 'gpt-6.1-terra' ? 'gpt-6.1-sol' : 'gpt-6.1-terra'
    target.dispatchEvent(new Event(event))
    await flush()
    assert.equal(chat.provider.model, selected, event)
  }
  visible.visibilityState = 'hidden'
  const beforeHidden = reads
  visible.dispatchEvent(new Event('visibilitychange'))
  viewport.dispatchEvent(new Event('focus'))
  await flush()
  assert.equal(reads, beforeHidden, 'hidden pages do not refresh')
  visible.visibilityState = 'visible'
  selected = 'menu-model'
  await chat.refreshProvider()
  assert.equal(chat.provider.model, selected, 'opening the model menu')
  selected = 'external-model'
  stream!.onmessage!({ data: JSON.stringify({ type: 'settingsChanged' }) })
  await flush()
  assert.equal(chat.provider.model, selected, 'existing stream reports a host edit')

  const beforeLocal = reads
  await api('/api/settings', { chatGPTModel: 'local-model' })
  assert.equal(chat.provider.model, 'local-model', 'local writes use the returned setting immediately')
  assert.equal(reads, beforeLocal, 'a local write needs no conversation read')

  accepted = 'accepted-model'
  const controller = chat as any
  controller.begin()
  await controller.request('/api/chat/send', { path: session.dir, session: 's' })
  await flush()
  assert.equal(chat.provider.model, accepted, 'turn start adopts the accepted model')
  await api('/api/settings', { chatGPTModel: 'next-turn-model' })
  await flush()
  assert.equal(chat.provider.model, accepted, 'edits during a turn preserve the accepted model')
  accepted = undefined
  controller.handle({ kind: 'finished' })
  await flush()
  assert.equal(chat.provider.model, 'next-turn-model', 'turn end shows the next selection')
  leave()
  const beforeLeave = reads
  viewport.dispatchEvent(new Event('focus'))
  visible.dispatchEvent(new Event('visibilitychange'))
  await flush()
  assert.equal(reads, beforeLeave, 'leaving removes presentation listeners')
  assert.equal(intervals, 0)
  // A run accepted through the session supersedes every helper's old flight
  // and source hold, even when no Agent `run` tool event reached this page.
  const actualNow = Date.now
  const now = 1234
  const after = now + 0.125
  chat.team.start([{ name: 'helper', cards: ['a'] }, { name: 'other', cards: ['b'] }], now)
  const writer = new LiveCodePlayer()
  writer.input(0, '{"value":{"path":"definitions/a/m.py","bytes":"old"', now)
  chat.writers.set('other', writer)
  const helper = chat.team.birds[0]
  const perch = (card: string) => ({ x: card === 'a' ? 0 : 500, y: 0 })
  helper.place(now, perch)
  helper.visit('b', 'reading', now + 0.01)
  helper.place(now + 0.01, perch)
  assert.ok(writer.at(after), 'the old source preview is still active before acceptance')
  assert.ok(helper.place(after, perch)?.flying)
  Date.now = () => after * 1000
  try {
    session.currentRunId = 'accepted-run'
    session.runActive = true
    session.changed()
    await flush()
  } finally { Date.now = actualNow }
  assert.equal(writer.at(after), undefined)
  assert.equal(helper.place(after, perch)?.flying, false)
  for (const bird of chat.team.birds) assert.ok(bird.line(after)?.text)
} finally {
  chat.close()
  globalThis.fetch = originalFetch
  globalThis.setInterval = originalInterval
}
console.log('chat model refresh: presentation, settings stream, local writes and turn boundaries passed without timers')

// Deliberately defer reads to reproduce the blank panel and out-of-order reloads.
{
  const original = globalThis.fetch
  const pending: { path: string; resolve(value: any): void; reject(error: Error): void }[] = []
  const textRow = (text: string) => ({ id: text, kind: 'text', role: 'assistant', text, name: '', arguments: '', result: '', state: '', archived: false, summaryJa: '', summaryEn: '', scopeJa: '', scopeEn: '', instruction: '', failed: 0, recovered: 0, links: [], children: [] })
  const snapshot = (id: string, text: string) => ({ conversation: { id, rows: text ? [textRow(text)] : [], messages: text ? [{ role: 'assistant', content: [{ type: 'text', text }] }] : [], archive: [], usage: {} }, provider: {}, busy: false })
  globalThis.fetch = ((path: string) => new Promise((resolve, reject) => pending.push({ path, resolve: value => resolve({ ok: true, json: async () => value } as Response), reject }))) as typeof fetch
  const c = Object.create(ChatController.prototype) as any
  Object.assign(c, { session: { dir: '/preserved' }, items: [{ id: 'original', role: 'assistant', text: 'original' }], currentId: 'old', displayedId: 'old', available: true, folders: [], turn: 0, reload: 0, contentRevision: 0, changed() {}, noteWriting() { return false } })
  try {
    c.select('next')
    assert.equal(c.items[0]?.text, 'original', 'switching keeps the visible conversation until its replacement is ready')
    assert.equal(c.ready, false, 'preserved rows cannot receive messages meant for the pending conversation')
    pending[0].reject(new Error('read failed'))
    await flush()
    assert.equal(c.items[0]?.text, 'original', 'a failed load never clears the conversation')
    assert.match(c.status, /read failed/)

    c.select('next')
    c.select('old')
    pending[2].resolve(snapshot('old', 'restored'))
    await flush()
    pending[1].resolve(snapshot('next', 'obsolete'))
    await flush()
    assert.equal(c.items[0]?.text, 'restored', 'a cancelled selection cannot replace the current conversation')
    assert.equal(c.ready, true)

    const reconnect = c.reread('old')
    c.handle({ kind: 'presentation', rows: [textRow('live progress')] })
    pending[3].resolve(snapshot('old', ''))
    await reconnect
    assert.equal(c.items[0]?.text, 'live progress', 'a reconnect snapshot cannot overwrite newer stream content with an empty list')
    const failedReconnect = c.reread('old')
    pending[4].reject(new Error('reconnect failed'))
    await failedReconnect
    assert.match(c.status, /reconnect failed/, 'reconnect errors stay visible')
    assert.equal(c.items[0]?.text, 'live progress')

    const retryRead = c.reread('old')
    pending[5].resolve(snapshot('old', 'live progress'))
    await retryRead
    assert.equal(c.status, undefined, 'a successful read clears its previous loading error')

    c.select('old')
    c.select('old')
    pending[7].resolve(snapshot('old', 'latest same ID'))
    await flush()
    pending[6].reject(new Error('obsolete error'))
    await flush()
    assert.equal(c.status, undefined, 'an obsolete error from a reselected ID cannot overwrite the latest state')
    assert.equal(c.items[0]?.text, 'latest same ID')

    const beforeListing = pending.length
    const listing = c.init()
    const list = pending.slice(beforeListing).find(request => request.path === '/api/chat/sessions')
    assert.ok(list)
    list.reject(new Error('session list failed'))
    await listing
    assert.ok(pending.slice(beforeListing).every(request => request.path === '/api/chat/sessions'),
      'a failed list does not load or create another conversation')
    assert.equal(c.items[0]?.text, 'latest same ID')
    assert.match(c.status, /session list failed/)
  } finally { globalThis.fetch = original }
}
console.log('chat content: pending/failed switches, cancelled loads, stale reconnects, same-ID races and list failures retain history')
