import assert from 'node:assert/strict'

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
const settings = () => ({ provider: 'chatgpt', chatGPTModel: selected, chatProvider: provider(), revision: { desktop: 1 } })
const flush = async () => { for (let i = 0; i < 40; i++) await Promise.resolve() }
globalThis.setInterval = ((...args: Parameters<typeof setInterval>) => { intervals++; return originalInterval(...args) }) as typeof setInterval
globalThis.fetch = (async (url, init) => {
  let value: unknown = {}
  if (url === '/api/chat/sessions') value = [{ id: 's', title: 'Chat', updatedAt: 0, messages: 0 }]
  if (url === '/api/skills') value = []
  if (url === '/api/chat/conversation') {
    reads++
    value = { provider: provider(), busy: !!accepted, conversation: { messages: [], archive: [], usage: {} } }
  }
  if (url === '/api/settings') {
    if (init?.body) selected = JSON.parse(String(init.body)).chatGPTModel ?? selected
    value = settings()
  }
  return { ok: true, json: async () => value } as Response
}) as typeof fetch
const { ChatController } = await import('../src/model/chat')
const { api } = await import('../src/api')
const chat = new ChatController({ dir: '/model-refresh' } as never, { status: { chatAvailable: true } } as never)
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
  stream!.onmessage!({ data: JSON.stringify({ type: 'chat', project: '/model-refresh', session: 's', event: { kind: 'modelAccepted' } }) })
  await flush()
  assert.equal(chat.provider.model, accepted, 'turn start adopts the accepted model')
  await api('/api/settings', { chatGPTModel: 'next-turn-model' })
  await flush()
  assert.equal(chat.provider.model, accepted, 'edits during a turn preserve the accepted model')
  accepted = undefined
  stream!.onmessage!({ data: JSON.stringify({ type: 'chat', project: '/model-refresh', session: 's', event: { kind: 'finished' } }) })
  await flush()
  assert.equal(chat.provider.model, 'next-turn-model', 'turn end shows the next selection')
  leave()
  const beforeLeave = reads
  viewport.dispatchEvent(new Event('focus'))
  visible.dispatchEvent(new Event('visibilitychange'))
  await flush()
  assert.equal(reads, beforeLeave, 'leaving removes presentation listeners')
  assert.equal(intervals, 0)
} finally {
  chat.close()
  globalThis.fetch = originalFetch
  globalThis.setInterval = originalInterval
}
console.log('chat model refresh: presentation, settings stream, local writes and turn boundaries passed without timers')
