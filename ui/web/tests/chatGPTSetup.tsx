import assert from 'node:assert/strict'
import { act, create } from 'react-test-renderer'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { modelErrorMessage } from '../src/i18n'
import { api } from '../src/api'
import { ChatGPTSignInModel, ChatGPTState, chatGPTSignInIsLocal } from '../src/model/chatgpt'
import { ChatGPTButton, ChatGPTSettings } from '../src/ui/chatgpt'
import { GuideSetupForm } from '../src/ui/guide'
import { GuideController } from '../src/model/guide'

// Track the real model operation even when its UI entry point returns void.
function tracked(model: ChatGPTSignInModel) {
  let pending = Promise.resolve()
  let status: Promise<ChatGPTState>
  const reload = model.reload.bind(model)
  model.reload = () => (status = reload())
  const run = model.run.bind(model)
  model.run = operation => (pending = run(operation))
  return Object.assign(model, { settled: () => pending, statusRead: () => status })
}
const account = { clientId: 'fixture', email: 'fixture@example.com', signedIn: true, planEnabled: true, active: true }
function fixture(local = true, welcomed = false) {
  let outcome: 'success' | 'no-plan' | 'cancelled' | 'failed' | 'pending' = 'success'
  let state: ChatGPTState = { accounts: [], signingIn: false, outcome: null, usageURL: 'https://chatgpt.com/settings/usage' }
  const attempts = new Map<string, ChatGPTState>()
  const calls: { path: string; body: any }[] = []
  const opened: string[] = []
  const tab = { opener: {}, location: { href: '' }, close() {} }
  let nextAttempt = 0
  let sharedReadError: Error | undefined
  type Event = { type: string; message?: string }
  const listeners = new Set<(event: Event) => void>()
  const observe = (fn: (event: Event) => void) => {
    listeners.add(fn)
    return () => { listeners.delete(fn) }
  }
  const finish = (result: Exclude<typeof outcome, 'pending'>) => {
    const ok = result === 'success' || result === 'no-plan'
    state = { ...state, signingIn: false,
      accounts: ok ? [{ ...account, planEnabled: result === 'success' }] : [],
      outcome: { ok, clientId: ok ? account.clientId : undefined, planEnabled: result === 'success',
        message: result, welcome: result === 'success' && !welcomed ? account.clientId : undefined } }
    attempts.set(state.attemptId!, state)
  }
  const notify = (type: string, message?: string) => {
    for (const fn of listeners) fn({ type, message })
  }
  const request = (async (path: string, body?: any) => {
    calls.push({ path, body })
    if (path === '/api/chatgpt/signin') {
      const attemptId = String(++nextAttempt)
      state = { ...state, signingIn: true, attemptId, outcome: null }
      attempts.set(attemptId, state)
      if (outcome !== 'pending') finish(outcome)
      return { url: 'https://fixture.invalid/authorize', attemptId }
    }
    if (path === '/api/chatgpt/cancel') {
      assert.equal(body.attemptId, state.attemptId)
      finish('cancelled')
      return {}
    }
    if (path === '/api/chatgpt/welcomed') {
      assert.equal(body.clientId, account.clientId)
      welcomed = true
      for (const [id, value] of attempts) {
        if (value.outcome?.welcome === body.clientId)
          attempts.set(id, { ...value, outcome: { ...value.outcome, welcome: undefined } })
      }
      state = { ...state, outcome: attempts.get(state.attemptId!)?.outcome ?? null }
      return {}
    }
    if (path.startsWith('/api/chatgpt?')) {
      const id = new URL(path, 'http://fixture').searchParams.get('attemptId')!
      assert.ok(attempts.has(id))
      return { ...attempts.get(id), accounts: state.accounts }
    }
    assert.equal(path, '/api/chatgpt')
    if (sharedReadError) {
      const error = sharedReadError
      sharedReadError = undefined
      throw error
    }
    return state
  }) as typeof api
  return { request, observe, calls, opened, finish, notify, setOutcome(v: typeof outcome) { outcome = v },
    setAccounts(accounts: ChatGPTState['accounts']) { state = { ...state, accounts } },
    failNextSharedRead() { sharedReadError = new Error('Shared status unavailable') },
    failAttempt() {
      attempts.set(state.attemptId!, { ...state, signingIn: false, outcome: { ok: false, message: 'Interrupted' } })
      state = { ...state, signingIn: false, attemptId: undefined }
    },
    completeAndNotify(type: string) {
      finish('success')
      notify(type)
    },
    model: () => tracked(new ChatGPTSignInModel(local, request, ((url: string) => { opened.push(url); return tab }) as never, observe)) }
}

for (const host of ['localhost', '127.0.0.1', '[::1]', '::1']) assert.equal(chatGPTSignInIsLocal(host), true)
for (const host of ['example.com', '192.168.1.2', 'localhost.example.com', '']) assert.equal(chatGPTSignInIsLocal(host), false)

// Actual first-start sheet offers the shared button alongside the existing key
// and Skip paths. SSR never runs effects or opens a browser.
{
  const html = renderToStaticMarkup(<GuideSetupForm />)
  assert.match(html, /data-oai-siwc-button=""/)
  assert.match(html, /type="password"/)
}
for (const local of [true, false]) for (const onboarding of [true, false]) {
  const f = fixture(local), model = f.model()
  const html = renderToStaticMarkup(<ChatGPTSettings onboarding={onboarding} controller={model} onConnected={() => assert.fail('render completed setup')} />)
  if (!local) {
    assert.match(html, /localhost/)
    assert.match(html, /disabled=""/)
    model.signIn()
    await model.settled()
    assert.deepEqual(f.calls, [])
    assert.deepEqual(f.opened, [])
  }
}

// Welcome is persisted through the same endpoint across setup and Settings.
{
  const f = fixture(), setup = f.model()
  let finished = 0
  setup.onConnected = () => { finished++ }
  setup.signIn()
  await setup.settled()
  assert.equal(setup.state?.outcome?.welcome, account.clientId)
  assert.equal(finished, 0, 'welcome must remain mounted before setup finishes')
  await setup.welcomed(false)
  await setup.welcomed(false)
  assert.equal(finished, 1)
  assert.equal(f.calls.filter(c => c.path.endsWith('/welcomed')).length, 1)
  const settings = f.model()
  settings.onConnected = () => { finished++ }
  settings.signIn(account.clientId)
  await settings.settled()
  assert.equal(settings.state?.outcome?.welcome, undefined)
  assert.equal(finished, 2)
  assert.deepEqual(f.opened, ['about:blank', 'about:blank'])
}
{
  const f = fixture(), model = f.model()
  model.signIn()
  await model.settled()
  await model.welcomed(true)
  assert.equal(f.opened.at(-1), model.state?.usageURL)
}
for (const outcome of ['no-plan', 'cancelled', 'failed'] as const) {
  const f = fixture(), model = f.model()
  f.setOutcome(outcome)
  model.onConnected = () => assert.fail(`${outcome} finished setup`)
  model.signIn()
  await model.settled()
  assert.equal(model.message, outcome)
  assert.equal(model.state?.signingIn, false)
  assert.equal(model.state?.outcome?.welcome, undefined)
  if (outcome === 'no-plan') {
    const html = renderToStaticMarkup(<ChatGPTSettings onboarding controller={model} onConnected={() => assert.fail()} />)
    let finished = 0
    model.onConnected = () => { finished++ }
    f.setOutcome('success')
    model.signIn(account.clientId, true)
    await model.settled()
    assert.equal(model.state.outcome?.welcome, account.clientId)
    assert.equal(finished, 0)
    await model.welcomed(false)
    assert.equal(finished, 1)
    assert.deepEqual(f.calls.findLast(c => c.path.endsWith('/signin'))?.body, { clientId: account.clientId, consent: true })
  }
}
{
  const f = fixture(), model = f.model()
  f.setOutcome('pending')
  model.signIn()
  await model.settled()
  const html = renderToStaticMarkup(<ChatGPTSettings onboarding controller={model} onConnected={() => assert.fail()} />)
  assert.equal(model.state?.signingIn, true)
  await model.cancel()
  assert.equal(model.message, 'cancelled')
  assert.equal(model.state?.signingIn, false)
}
// A callback that arrives after the begin response is observed through
// the event stream. Reconnecting also recovers a missed completion.
for (const type of ['chatgptChanged', 'reconnected', 'eventsConnected']) {
  const f = fixture(), model = f.model()
  f.setOutcome('pending')
  await model.mount()
  model.signIn()
  await model.settled()
  assert.equal(model.state.signingIn, true)
  f.completeAndNotify(type)
  await model.statusRead()
  assert.equal(model.state.signingIn, false)
  assert.equal(model.state.outcome?.welcome, account.clientId)
  model.dispose()
}
// A single completion event survives a failed shared read, and the retry
// control clears the error without another notification or sign-in.
{
  const f = fixture(true, true), model = f.model()
  f.setOutcome('pending')
  let ui!: ReturnType<typeof create>
  act(() => {
    ui = create(<ChatGPTSettings controller={model} onConnected={() => {}} />)
  })
  await model.statusRead()
  model.signIn()
  await model.settled()
  f.failNextSharedRead()
  f.completeAndNotify('chatgptChanged')
  await model.statusRead()
  assert.equal(model.state.signingIn, false)
  assert.equal(model.state.outcome?.clientId, account.clientId)
  assert.ok(model.connectionError)
  const retry = ui.root.findByProps({ role: 'status' }).parent!.findByType('button')
  retry.props.onClick({ stopPropagation() {} })
  await model.statusRead()
  assert.equal(model.connectionError, undefined)
  assert.equal(model.state.outcome?.clientId, account.clientId)
  ui.unmount()
}
// Tab A keeps its outcome even after tab B starts the next sign-in.
for (const welcomed of [true, false]) {
  const f = fixture(true, welcomed), a = f.model(), b = f.model()
  let connected = 0
  a.onConnected = () => { connected++ }
  await a.mount()
  f.setOutcome('pending')
  a.signIn()
  await a.settled()
  const attempt = a.state.attemptId
  f.finish('success')
  await b.reload()
  b.signIn()
  await b.settled()
  f.notify('chatgptChanged')
  await a.statusRead()
  assert.ok(f.calls.some(c => c.path === `/api/chatgpt?attemptId=${attempt}`))
  assert.equal(a.state.attemptId, b.state.attemptId)
  assert.equal(a.state.signingIn, true)
  assert.equal(a.url, undefined)
  assert.equal(a.message, 'success')
  if (!welcomed) {
    assert.equal(connected, 0)
    assert.equal(a.state.outcome?.welcome, account.clientId)
    await a.welcomed(false)
  }
  assert.equal(connected, 1)
  a.dispose()
  assert.equal(b.state.signingIn, true)
  assert.ok(!f.calls.some(c => c.path.endsWith('/cancel')))
  await b.cancel()
}
// Selecting an account can return after the callback has completed.
{
  const f = fixture()
  const selected = Promise.withResolvers<void>(), respond = Promise.withResolvers<void>()
  const request = (async (path: string, body?: any) => {
    if (path === '/api/chatgpt/select') {
      const pending = await f.request('/api/chatgpt')
      selected.resolve()
      await respond.promise
      return pending
    }
    return f.request(path, body)
  }) as typeof api
  const model = tracked(new ChatGPTSignInModel(true, request, (() => null) as never, f.observe))
  await model.mount()
  f.setOutcome('pending')
  model.signIn()
  await model.settled()
  const action = model.updateAccount('select', account.clientId)
  await selected.promise
  f.completeAndNotify('chatgptChanged')
  await model.statusRead()
  assert.equal(model.state.signingIn, false)
  respond.resolve()
  await action
  assert.equal(model.state.signingIn, false)
  assert.equal(model.state.outcome?.welcome, account.clientId)
  model.dispose()
}
// A follow-up setup action can fail after an event delivers sign-in success.
{
  const f = fixture(true, true), model = f.model()
  await model.mount()
  f.setOutcome('pending')
  model.onConnected = () => { throw new Error('Setup could not be saved') }
  model.signIn()
  await model.settled()
  f.completeAndNotify('chatgptChanged')
  await model.settled()
  assert.match(model.message!, /Setup could not be saved/)
  assert.equal(model.state.signingIn, false)
  model.dispose()
}
// The stream can fail to reattach once. A later authenticated stream
// resynchronizes and clears a pending attempt lost on Host restart.
{
  const f = fixture(), model = f.model()
  await model.mount()
  f.setOutcome('pending')
  model.signIn()
  await model.settled()
  f.notify('hostUnavailable', 'Host unavailable')
  assert.ok(model.connectionError)
  f.failAttempt()
  f.notify('reconnected')
  await model.statusRead()
  assert.equal(model.state.signingIn, false)
  assert.equal(model.connectionError, undefined)
  assert.equal(model.url, undefined)
  assert.equal(model.message, 'Interrupted')
  model.dispose()
}
// A slow initial status read cannot re-enable sign-in while begin is pending.
{
  const f = fixture()
  f.setOutcome('pending')
  let begin!: () => void
  const request = (async (path: string, body?: any) => {
    if (path === '/api/chatgpt/signin') {
      const result = await f.request(path, body)
      await new Promise<void>(resolve => { begin = resolve })
      return result
    }
    return f.request(path, body)
  }) as typeof api
  const model = tracked(new ChatGPTSignInModel(true, request, (() => null) as never))
  const initial = model.reload()
  model.signIn()
  await initial
  assert.equal(model.state.signingIn, true)
  model.signIn()
  assert.equal(f.calls.filter(c => c.path.endsWith('/signin')).length, 1)
  await model.cancel()
  begin()
  await model.settled()
  assert.equal(model.message, 'cancelled')
  assert.equal(model.state.signingIn, false)
}
// Run the shared sheet content through a real React mount/unmount. Cleanup
// must reach the host, including when begin has not returned an attempt yet.
for (const beginPending of [false, true]) {
  const f = fixture()
  f.setOutcome('pending')
  const beginEntered = Promise.withResolvers<void>()
  let finishBegin!: () => void
  const request = (async (path: string, body?: unknown) => {
    const result = await f.request(path, body)
    if (path.endsWith('/signin') && beginPending) {
      await new Promise<void>(resolve => { finishBegin = resolve; beginEntered.resolve() })
    }
    return result
  }) as typeof api
  const tab = { opener: {}, location: { href: '' }, closed: false, close() { this.closed = true } }
  const model = tracked(new ChatGPTSignInModel(true, request, (() => tab) as never, f.observe))
  let mounted!: ReturnType<typeof create>
  act(() => {
    mounted = create(<ChatGPTSettings onboarding controller={model} onConnected={() => assert.fail('unmounted setup completed')} />)
  })
  await model.statusRead()
  act(() => {
    mounted.root.findByProps({ 'data-oai-siwc-button': '' }).props.onClick()
  })
  if (beginPending) await beginEntered.promise
  else await model.settled()
  assert.equal(model.state.signingIn, true)
  act(() => { mounted.unmount() })
  if (beginPending) {
    assert.equal(f.calls.filter(c => c.path.endsWith('/cancel')).length, 0)
    finishBegin()
  }
  await model.settled()
  assert.equal(f.calls.filter(c => c.path.endsWith('/cancel')).length, 1)
  assert.equal(model.state.signingIn, false)
  if (beginPending) {
    assert.equal(tab.closed, true)
    assert.equal(tab.location.href, '', 'a dismissed begin must not navigate the browser')
  }
}
// A reload at the welcome creates a model with no transient completion flag.
// Its restored successful result must still run the real setup completion.
{
  const f = fixture(), first = f.model()
  first.signIn()
  await first.settled()
  assert.equal(first.state.outcome?.welcome, account.clientId)
  const reloaded = f.model()
  await reloaded.reload()
  const original = globalThis.fetch
  const calls: { path: string; body: any }[] = []
  globalThis.fetch = (async (path: string, init: RequestInit) => {
    const body = JSON.parse(String(init?.body ?? '{}'))
    calls.push({ path, body })
    const value = path === '/api/settings' ? { revision: 'host:1' }
      : path === '/api/guide/advance' ? { setupDone: true }
      : path === '/api/guide' ? { available: true, setupDone: true, demoAnswered: true }
      : assert.fail(`Unexpected API ${path}`)
    return { ok: true, json: async () => value }
  }) as typeof fetch
  try {
    const guide = new GuideController()
    reloaded.onConnected = clientId => guide.finishChatGPTSetup(clientId)
    await reloaded.welcomed(false)
    await reloaded.welcomed(false)
    assert.equal(calls.filter(c => c.path === '/api/settings' && c.body.provider === 'chatgpt').length, 1)
    assert.equal(guide.setupDone, true)
    assert.equal(guide.connected, true)
    assert.equal(f.calls.filter(c => c.path.endsWith('/welcomed')).length, 1)
  } finally { globalThis.fetch = original }
}
// Begin failure must leave a retryable button rather than a permanent spinner.
{
  const model = tracked(new ChatGPTSignInModel(true, (async () => { throw Error('fixture begin failure') }) as typeof api, (() => null) as never))
  model.signIn()
  await model.settled()
  assert.equal(model.state?.signingIn, false)
  assert.match(model.message!, /fixture begin failure/)
}
// The setup request names the account that actually completed sign-in. A
// second tab can change the current registration before the welcome is closed.
for (const switched of [false, true]) {
  const f = fixture(), model = f.model()
  await model.mount()
  model.signIn()
  await model.settled()
  const other = { ...account, clientId: 'other', planEnabled: false }
  const accounts = switched ? [{ ...account, active: false }, other] : [account]
  f.setAccounts(accounts)
  f.notify('chatgptChanged')
  await model.statusRead()
  assert.equal(model.state.outcome?.welcome, account.clientId)
  const original = globalThis.fetch
  const calls: { path: string; body: any }[] = []
  globalThis.fetch = (async (path: string, init: RequestInit) => {
    const body = JSON.parse(String(init?.body ?? '{}'))
    calls.push({ path, body })
    if (path === '/api/settings' && body.provider) {
      assert.equal(body.chatGPTAccount, account.clientId)
      if (!accounts.some(a => a.clientId === body.chatGPTAccount && a.active && a.signedIn && a.planEnabled))
        return { ok: false, json: async () => ({ code: 'chatgpt_account_changed', error: 'Account changed' }) }
    }
    const value = path === '/api/settings' ? { revision: 'host:1' }
      : path === '/api/guide/advance' ? { setupDone: true }
      : path === '/api/guide' ? { available: true, setupDone: true, demoAnswered: true }
      : assert.fail(`Unexpected API ${path}`)
    return { ok: true, json: async () => value }
  }) as typeof fetch
  try {
    const g = new GuideController()
    g.setupDone = false
    model.onConnected = clientId => g.finishChatGPTSetup(clientId)
    await model.welcomed(false)
    assert.equal(calls.find(c => c.path === '/api/settings' && c.body.provider)?.body.provider, 'chatgpt')
    assert.equal(calls.find(c => c.path === '/api/guide/advance')?.body.event.kind, switched ? undefined : 'setup')
    assert.equal(!!g.setupDone, !switched)
    assert.equal(!!g.connected, !switched)
    if (switched) assert.ok(model.message && model.message !== model.state.outcome?.message)
  } finally {
    model.dispose()
    globalThis.fetch = original
  }
}

// An expired registration offers sign-in for that account.
{
  const f = fixture()
  f.setOutcome('pending')
  const model = f.model()
  f.setAccounts([account])
  let ui!: ReturnType<typeof create>
  act(() => {
    ui = create(<ChatGPTSettings controller={model} onConnected={() => {}} />)
  })
  await model.statusRead()
  try {
    f.setAccounts([{ ...account, signedIn: false }])
    f.notify('chatgptChanged')
    await model.statusRead()
    assert.equal(model.state?.accounts.find(a => a.clientId === account.clientId)?.signedIn, false)
    const signIn = ui.root.findAllByType(ChatGPTButton).find(button => {
      const row = button.parent
      return row?.findAllByType('span').length
    })
    assert.ok(signIn && !signIn.props.disabled)
    signIn.props.onClick()
    await model.settled()
    assert.equal(f.calls.find(c => c.path === '/api/chatgpt/signin')?.body.clientId, account.clientId)
  } finally { ui.unmount() }
  assert.equal(modelErrorMessage('Credentials expired', 'chatgpt_sign_in_required'), 'ChatGPT にもう一度サインインしてください。')
  const detail = 'model returned HTTP 503: Capacity exhausted'
  assert.equal(modelErrorMessage(detail), detail)
}
{
  const original = globalThis.fetch
  globalThis.fetch = (async () => ({ ok: false, json: async () => ({ error: 'Credentials expired', code: 'chatgpt_sign_in_required' }) })) as typeof fetch
  try {
    await assert.rejects(api('/api/settings/models'), (error: Error) =>
      error.message === modelErrorMessage('Credentials expired', 'chatgpt_sign_in_required'))
  } finally { globalThis.fetch = original }
}
console.log('ChatGPT first start and Settings: shared button, success, consent, cancellation, failures, non-local page and once-only welcome passed')
