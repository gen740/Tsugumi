// The browser projects core replies; it has no script or transition fixtures.
import assert from 'node:assert/strict'
const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { GuideController } = await import('../src/model/guide')
const calls: { url: string; body: any }[] = []
const realFetch = globalThis.fetch
const tour = { demo: 'report', title: 'Report', path: '/copy', started: true }
globalThis.fetch = (async (url: string, init: RequestInit) => {
  const body = JSON.parse(String(init.body ?? '{}'))
  calls.push({ url: String(url), body })
  const state = { revision: calls.length, answerRevision: calls.length, setupDone: true, demoAnswered: true,
    step: 'server-step', text: 'Server reply', say: 'Server bubble', present: true, tour }
  const value = String(url) === '/api/guide' ? { ...state, available: false, busy: false, conversation: null }
    : body.event.kind === 'action_finished' ? { ...state, present: false } : state
  return { ok: true, status: 200, json: async () => value }
}) as typeof fetch
try {
  const guide = new GuideController()
  await guide.refresh()
  assert.equal(guide.scripted, true)
  assert.equal(guide.tour?.path, '/copy')
  await guide.answerScripted('Question')
  assert.equal(guide.items.at(-1)?.text, 'Server reply')
  assert.deepEqual(guide.bubble?.choices, [])
  assert.deepEqual(guide.bubble?.actions, [])
  assert.equal(calls.at(-1)?.body.event.expected_step, 'server-step')
  assert.equal(calls.at(-1)?.body.event.kind, 'message')
  assert.equal(calls.at(-1)?.body.event.scripted, true)
  const shown: unknown[] = []
  const chat = { session: { dir: '/copy', scopeName: () => 'Report', connected: true, runActive: false },
    showScripted: (...args: unknown[]) => shown.push(args) }
  await guide.answerScripted('$demo', chat as any)
  assert.equal(calls.at(-1)?.body.event.title, 'Report')
  assert.equal(calls.at(-1)?.body.event.project, '/copy')
  assert.equal(shown.length, 1)
  assert.equal(guide.speakingChat, chat)
  assert.ok(calls.every((c) => !c.url.includes('/send') && !c.url.includes('/state')))
} finally {
  globalThis.fetch = realFetch
}
console.log('Guide UI: core events and presentation, absent lists, no model or local state endpoint passed')
