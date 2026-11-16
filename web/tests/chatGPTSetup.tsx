import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
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
  assert.match(html, /Continue with ChatGPT/)
  assert.match(html, /type="password"/)
  assert.match(html, /スキップして始める|Skip and Start/)
}
for (const local of [true, false]) for (const onboarding of [true, false]) {
  const f = fixture(local), model = f.model()
  const html = renderToStaticMarkup(<ChatGPTSettings onboarding={onboarding} controller={model} onConnected={() => assert.fail('render completed setup')} />)
  assert.match(html, /Continue with ChatGPT/)
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
    assert.match(html, /プランの利用を許可|Allow Plan Use/)
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
  assert.match(html, /ブラウザでサインインを続けてください|Continue signing in/)
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
console.log('ChatGPT first start and Settings: shared button, success, consent, cancellation, failures, non-local page and once-only welcome passed')
