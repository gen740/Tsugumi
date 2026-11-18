import assert from 'node:assert/strict'
import { act, create } from 'react-test-renderer'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { modelErrorMessage } from '../src/i18n'
import { api } from '../src/api'
import { ChatGPTSignInModel, ChatGPTState, chatGPTSignInIsLocal } from '../src/model/chatgpt'
import { ChatGPTSettings } from '../src/ui/chatgpt'
import { GuideSetupForm } from '../src/ui/guide'
import { GuideController } from '../src/model/guide'

const tick = () => new Promise<void>(resolve => setTimeout(resolve, 0))
const account = { clientId: 'fixture', email: 'fixture@example.com', signedIn: true, planEnabled: true, active: true }
function fixture(local = true) {
  let welcomed = false
  let outcome: 'success' | 'no-plan' | 'cancelled' | 'failed' | 'pending' = 'success'
  let state: ChatGPTState = { accounts: [], signingIn: false, outcome: null, usageURL: 'https://chatgpt.com/settings/usage' }
  const calls: { path: string; body: any }[] = []
  const opened: string[] = []
  const tab = { opener: {}, location: { href: '' }, close() {} }
  const request = (async (path: string, body?: any) => {
    calls.push({ path, body })
    if (path === '/api/chatgpt/signin') {
      state = { ...state, signingIn: true, outcome: null }
      return { url: 'https://fixture.invalid/authorize' }
    }
    if (path === '/api/chatgpt/cancel') { outcome = 'cancelled'; return state }
    if (path === '/api/chatgpt/welcomed') {
      assert.equal(body.clientId, account.clientId)
      welcomed = true
      state = { ...state, outcome: { ...state.outcome!, welcome: undefined } }
      return state
    }
    assert.equal(path, '/api/chatgpt')
    if (state.signingIn && outcome !== 'pending') {
      const ok = outcome === 'success' || outcome === 'no-plan'
      state = { ...state, signingIn: false,
        accounts: ok ? [{ ...account, planEnabled: outcome === 'success' }] : [],
        outcome: { ok, message: outcome, welcome: outcome === 'success' && !welcomed ? account.clientId : undefined } }
    }
    return state
  }) as typeof api
  return { request, calls, opened, setOutcome(v: typeof outcome) { outcome = v },
    model: () => new ChatGPTSignInModel(local, request, ((url: string) => { opened.push(url); return tab }) as never) }
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
    await tick()
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
  await tick()
  assert.equal(setup.state?.outcome?.welcome, account.clientId)
  assert.equal(finished, 0, 'welcome must remain mounted before setup finishes')
  await setup.welcomed(false)
  await setup.welcomed(false)
  assert.equal(finished, 1)
  assert.equal(f.calls.filter(c => c.path.endsWith('/welcomed')).length, 1)
  const settings = f.model()
  settings.onConnected = () => { finished++ }
  settings.signIn(account.clientId)
  await tick()
  assert.equal(settings.state?.outcome?.welcome, undefined)
  assert.equal(finished, 2)
  assert.deepEqual(f.opened, ['about:blank', 'about:blank'])
}
{
  const f = fixture(), model = f.model()
  model.signIn()
  await tick()
  await model.welcomed(true)
  assert.equal(f.opened.at(-1), model.state?.usageURL)
}
for (const outcome of ['no-plan', 'cancelled', 'failed'] as const) {
  const f = fixture(), model = f.model()
  f.setOutcome(outcome)
  model.onConnected = () => assert.fail(`${outcome} finished setup`)
  model.signIn()
  await tick()
  assert.equal(model.message, outcome)
  assert.equal(model.state?.signingIn, false)
  assert.equal(model.state?.outcome?.welcome, undefined)
  if (outcome === 'no-plan') {
    const html = renderToStaticMarkup(<ChatGPTSettings onboarding controller={model} onConnected={() => assert.fail()} />)
    let finished = 0
    model.onConnected = () => { finished++ }
    f.setOutcome('success')
    model.signIn(account.clientId, true)
    await tick()
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
  await tick()
  const html = renderToStaticMarkup(<ChatGPTSettings onboarding controller={model} onConnected={() => assert.fail()} />)
  assert.equal(model.state?.signingIn, true)
  await model.cancel()
  assert.equal(model.message, 'cancelled')
  assert.equal(model.state?.signingIn, false)
}
// A slow initial status read cannot re-enable sign-in while begin is pending.
{
  const f = fixture()
  let begin!: (value: { url: string }) => void
  const request = (async (path: string, body?: any) => {
    if (path === '/api/chatgpt/signin') {
      await f.request(path, body)
      return new Promise(resolve => { begin = resolve })
    }
    return f.request(path, body)
  }) as typeof api
  const model = new ChatGPTSignInModel(true, request, (() => null) as never)
  const initial = model.reload()
  model.signIn()
  await initial
  assert.equal(model.state.signingIn, true)
  model.signIn()
  assert.equal(f.calls.filter(c => c.path.endsWith('/signin')).length, 1)
  await model.cancel()
  begin({ url: 'https://fixture.invalid/authorize' })
  await tick()
  assert.equal(model.message, 'cancelled')
  assert.equal(model.state.signingIn, false)
}
// Run the shared sheet content through a real React mount/unmount. Cleanup
// must reach the host, including when begin has not returned an attempt yet.
for (const beginPending of [false, true]) {
  const f = fixture()
  f.setOutcome('pending')
  let finishBegin!: () => void
  const request = (async (path: string, body?: unknown) => {
    const result = await f.request(path, body)
    if (path.endsWith('/signin') && beginPending) {
      await new Promise<void>(resolve => { finishBegin = resolve })
    }
    return result
  }) as typeof api
  const tab = { opener: {}, location: { href: '' }, closed: false, close() { this.closed = true } }
  const model = new ChatGPTSignInModel(true, request, (() => tab) as never)
  let mounted!: ReturnType<typeof create>
  act(() => {
    mounted = create(<ChatGPTSettings onboarding controller={model} onConnected={() => assert.fail('unmounted setup completed')} />)
  })
  await tick()
  act(() => {
    mounted.root.findByProps({ 'data-oai-siwc-button': '' }).props.onClick()
  })
  await tick()
  assert.equal(model.state.signingIn, true)
  act(() => { mounted.unmount() })
  if (beginPending) {
    assert.equal(f.calls.filter(c => c.path.endsWith('/cancel')).length, 0)
    finishBegin()
  }
  await tick()
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
  await tick()
  assert.equal(first.state.outcome?.welcome, account.clientId)
  const reloaded = f.model()
  await reloaded.reload()
  const original = globalThis.fetch
  const calls: { path: string; body: any }[] = []
  globalThis.fetch = (async (path: string, init: RequestInit) => {
    const body = JSON.parse(String(init?.body ?? '{}'))
    calls.push({ path, body })
    const value = path === '/api/settings' ? { revision: { desktop: 1 } }
      : path === '/api/guide/advance' ? { setupDone: true }
      : path === '/api/guide' ? { available: true, setupDone: true, demoAnswered: true }
      : assert.fail(`Unexpected API ${path}`)
    return { ok: true, json: async () => value }
  }) as typeof fetch
  try {
    const guide = new GuideController()
    reloaded.onConnected = () => guide.finishChatGPTSetup()
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
  const model = new ChatGPTSignInModel(true, (async () => { throw Error('fixture begin failure') }) as typeof api, (() => null) as never)
  model.signIn()
  await tick()
  assert.equal(model.state?.signingIn, false)
  assert.match(model.message!, /fixture begin failure/)
}
// Exercise the real setup completion: select the provider, mark setup done,
// refresh as connected. The API double rejects every unexpected operation.
{
  const original = globalThis.fetch
  const calls: { path: string; body: any }[] = []
  globalThis.fetch = (async (path: string, init: RequestInit) => {
    const body = JSON.parse(String(init?.body ?? '{}'))
    calls.push({ path, body })
    const value = path === '/api/settings' ? { revision: { desktop: 1 } }
      : path === '/api/guide/advance' ? { setupDone: true }
      : path === '/api/guide' ? { available: true, setupDone: true, demoAnswered: true }
      : assert.fail(`Unexpected API ${path}`)
    return { ok: true, json: async () => value }
  }) as typeof fetch
  try {
    const g = new GuideController()
    await g.finishChatGPTSetup()
    assert.equal(calls.find(c => c.path === '/api/settings' && c.body.provider)?.body.provider, 'chatgpt')
    assert.equal(calls.find(c => c.path === '/api/guide/advance')?.body.event.kind, 'setup')
    assert.equal(g.setupDone, true)
    assert.equal(g.connected, true)
  } finally { globalThis.fetch = original }
}

// Expired sessions expose the recovery action in both Settings and guide errors.
{
  const model = fixture().model()
  model.setState({ ...model.state, accounts: [{ ...account, signedIn: false }] })
  const html = renderToStaticMarkup(<ChatGPTSettings controller={model} onConnected={() => {}} />)
  assert.match(html, /ChatGPT にもう一度サインインしてください。/)
  assert.equal(modelErrorMessage('Credentials expired', 'chatgpt_sign_in_required'), 'ChatGPT にもう一度サインインしてください。')
  assert.equal(modelErrorMessage('model returned HTTP 503: Capacity exhausted'), 'model returned HTTP 503: Capacity exhausted')
}
{
  const original = globalThis.fetch
  globalThis.fetch = (async () => ({ ok: false, json: async () => ({ error: 'Credentials expired', code: 'chatgpt_sign_in_required' }) })) as typeof fetch
  try {
    await assert.rejects(api('/api/settings/models'), { message: 'ChatGPT にもう一度サインインしてください。' })
  } finally { globalThis.fetch = original }
}
console.log('ChatGPT first start and Settings: shared button, success, consent, cancellation, failures, non-local page and once-only welcome passed')
