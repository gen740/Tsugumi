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
let available = false
let connectionMessage: string | undefined
let failedReads = 0
let sent: (() => void) | undefined
const tour = { demo: 'report', title: 'Report', path: '/copy', started: true }
globalThis.fetch = (async (url: string, init: RequestInit) => {
  const body = JSON.parse(String(init.body ?? '{}'))
  calls.push({ url: String(url), body })
  if (String(url) === '/api/guide' && failedReads > 0) {
    failedReads--
    throw new Error('Host restarting')
  }
  const state = { revision: calls.length, answerRevision: calls.length, setupDone: true, demoAnswered: true,
    step: 'server-step', text: 'Server reply', say: 'Server bubble', present: true, tour }
  const value = String(url) === '/api/guide' ? { ...state, available, connectionMessage, busy: false, conversation: null }
    : String(url) === '/api/guide/send' ? (sent?.(), {})
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
  // The next question checks again and reaches the model after recovery.
  available = true
  const delivered = new Promise<void>((resolve) => { sent = resolve })
  await guide.answerScripted('Recovered question')
  await delivered
  assert.equal(guide.connected, true)
  assert.equal(calls.at(-1)?.url, '/api/guide/send')
  assert.equal(calls.at(-1)?.body.text, 'Recovered question')
  // A failed read is retried once and never changes a known connection to false.
  const beforeRetry = calls.length
  failedReads = 1
  assert.equal(await guide.refresh(), true)
  assert.equal(calls.length - beforeRetry, 2)
  failedReads = 2
  assert.equal(await guide.refresh(), false)
  assert.equal(guide.connected, true)
  const beforeFailure = calls.length
  failedReads = 2
  await guide.answerScripted('Question during restart')
  assert.equal(calls.length - beforeFailure, 2)
  assert.ok(!calls.slice(beforeFailure).some(c => c.url === '/api/guide/advance'))
  // A delayed old status cannot override the fresh question's check.
  const request = globalThis.fetch
  let releaseOld!: () => void
  let requested!: () => void
  const oldRequested = new Promise<void>((resolve) => { requested = resolve })
  let oldRead = true
  globalThis.fetch = (async (...args: Parameters<typeof fetch>) => {
    if (String(args[0]) === '/api/guide' && oldRead) {
      oldRead = false
      requested()
      await new Promise<void>((resolve) => { releaseOld = resolve })
      return { ok: true, status: 200, json: async () => ({ available: false, busy: false }) } as Response
    }
    return request(...args)
  }) as typeof fetch
  const outdated = guide.refresh()
  await oldRequested
  const recovered = new Promise<void>((resolve) => { sent = resolve })
  await guide.answerScripted('Fresh question')
  await recovered
  releaseOld()
  assert.equal(await outdated, false)
  assert.equal(guide.connected, true)
  globalThis.fetch = request
  available = false
  connectionMessage = 'Sign in to ChatGPT again'
  const beforeSignIn = calls.length
  await guide.answerScripted('Question with revoked registration')
  assert.equal(guide.items.at(-1)?.text, connectionMessage)
  assert.equal(guide.bubble?.actions[0]?.kind, 'open_settings')
  assert.ok(!calls.slice(beforeSignIn).some(c => c.url === '/api/guide/advance'))
} finally {
  globalThis.fetch = realFetch
}
console.log('Guide UI: core events and presentation, absent lists, no model or local state endpoint passed')

// A browser with an old connected snapshot still receives the sign-in reason.
{
  const { chatRoutes } = await import('../src/host/chat')
  const { CoreHost } = await import('../src/host/coreHost')
  const original = { settings: CoreHost.prototype.getSettings }
  try {
    for (const language of ['ja', 'en']) {
      CoreHost.prototype.getSettings = async function () {
        this.user.call = async <T = any>(method: string): Promise<T> => {
          if (method === 'ModelSelectionRequest') return { provider: { providerId: 'chatgpt', profile: 'desktop', model: 'fixture' } } as T
          if (method === 'ProviderStatusRequest') return { configured: false, code: 'chatgpt_sign_in_required' } as T
          if (method === 'GuideAdvanceRequest') return {} as T
          throw new Error(`Unexpected request: ${method}`)
        }
        return { revision: 'host:0', values: Object.entries({ 'agent.provider': 'chatgpt', 'agent.model.chatgpt': 'fixture', 'app.language': language })
          .map(([key, value]) => ({ key, value_json: JSON.stringify(value) })) }
      }
      const routes = chatRoutes({ envs: {} as never, projectDir: String,
        broadcast: () => {}, httpError: (status, message) => Object.assign(new Error(message), { status }) })
      // The page's own language (it adopts the setting at load), not the setting read here.
      const message = 'ChatGPT にもう一度サインインしてください。'
      const state = await routes['POST /api/guide']({}, new URL('http://fixture')) as any
      assert.equal(state.connectionMessage, message)
      await assert.rejects(routes['POST /api/guide/send']({ text: 'Question' }, new URL('http://fixture')) as Promise<unknown>,
        (error: any) => error.status === 409 && error.message === message)
    }
  } finally {
    CoreHost.prototype.getSettings = original.settings
  }
}
