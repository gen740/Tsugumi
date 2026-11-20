import { Access } from '../shared/gen/tsugumi/service/access'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { chatRoutes, type ChatContext } from '../server/chat'
import { CoreHost } from '../server/coreHost'
import { toolStep, PermissionRequests } from '../src/ui/chat'
import { permissionCatalog } from '../src/model/permissions'
import { projectAccessNeededNow } from '../shared/agentShared'
import { act, create } from 'react-test-renderer'
import { createElement } from 'react'

// The HTTP adapter must scope lists/search and adopt host edits and the
// accepted turn's model. No socket, model or environment is used here.
const dir = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-host-copies-')))
const folders = ['a', 'b'].map(n => path.join(dir, n))
for (const folder of folders) {
  fs.mkdirSync(folder)
  fs.writeFileSync(path.join(folder, 'tsugumi.json'), '{"name":"Collatz"}')
}
const original = { settings: CoreHost.prototype.getSettings, environment: CoreHost.prototype.provisionEnvironment,
  project: CoreHost.prototype.project }
let model = 'gpt-6.1-sol'
let busy = false
let signInState = 'idle'
const asked = 'project_access ' + JSON.stringify({ title: 'Collatz', path: folders[0], access: 'Read', reason: 'Read project content', needed_now: true })
const write = 'project_access ' + JSON.stringify({ title: 'Collatz', path: folders[0], access: 'Write', needed_now: false })
const run = 'project_access ' + JSON.stringify({ title: 'Collatz', path: folders[0], access: 'Run', needed_now: false })
let pendingAsked = [asked, write, run]
const calls: { method: string; fields: any }[] = []
const events: any[] = []
let host: CoreHost | undefined
const conversations = new Map<string, string>()
const snapshots = (id: string) => ({ conversation: { id, messages: [], archive: [], tools: [], usage: { inputTokens: 0, outputTokens: 0 } },
  busy, modelBinding: { providerId: 'chatgpt', model: 'gpt-6.1-terra', effort: 'high', fast: false }, event_seq: 0n })
try {
  CoreHost.prototype.provisionEnvironment = async () => {}
  CoreHost.prototype.project = async function(folder) { return folder === folders[0] ? 'project-a' : 'project-b' }
  CoreHost.prototype.getSettings = async function() {
    host = this
    this.user.call = async <T>(method: string, fields: any = {}): Promise<T> => {
      calls.push({ method, fields })
      if (method === 'SignInBeginRequest') {
        signInState = 'pending'
        return { url: 'https://fixture.invalid/authorize', attempt_id: 'attempt-a' } as T
      }
      if (method === 'SignInStatusRequest') {
        return { accounts: [], attempt_id: 'attempt-a', state: signInState } as T
      }
      if (method === 'SignInActionRequest' && fields.action === 'cancel') {
        assert.equal(fields.attempt_id, 'attempt-a')
        signInState = 'cancelled'
        return {} as T
      }
      if (method === 'AccessResolveRequest') return {} as T
      if (method === 'ModelSelectionRequest') return { provider: { providerId: 'chatgpt', model: 'gpt-6.1-luna' } } as T
      if (method === 'ModelsListRequest') return [] as T
      if (method === 'ConversationListRequest') return (fields.project_id === 'project-a' ? [{ id: 'a', title: 'Only A' }] : []) as T
      if (method === 'ConversationSearchRequest') return [] as T
      if (method === 'ConversationCreateRequest') {
        const project = conversations.get(fields.conversation_id)
        if (project && project !== fields.project_id) throw new Error('conflict')
        conversations.set(fields.conversation_id, fields.project_id)
        return snapshots(fields.conversation_id) as T
      }
      if (method === 'ConversationGetRequest') return snapshots(fields.conversation_id) as T
      if (method === 'AccessListRequest') return { requests: [{
        id: 'pending-a', state: 'pending', conversation_id: 'a', project_path: folders[0], asked: pendingAsked,
      }] } as T
      throw new Error(`Unexpected call: ${method}`)
    }
    return { revision: 'host:1', values: Object.entries({ 'agent.provider': 'chatgpt', 'agent.model.chatgpt': model,
      'agent.effort': '', 'agent.fastModel': '', 'agent.skillFolders': [], 'app.language': 'en' })
      .map(([key, value]) => ({ key, value_json: JSON.stringify(value) })) }
  }
  const routes = chatRoutes({ support: dir, here: dir, desktop: true, envs: {} as never, projectDir: String,
    broadcast(event) { events.push(event) }, httpError: (_status, message) => new Error(message) } as ChatContext)
  const call = async (route: string, fields: any) => routes[`POST /api/chat/${route}`](fields, new URL('http://fixture')) as Promise<any>
  assert.deepEqual(await call('sessions', { path: folders[0] }), [{ id: 'a', title: 'Only A' }])
  host!.event({ type: 'SettingsChanged', value: { revision: 'host:2' } })
  await new Promise(resolve => setImmediate(resolve))
  assert.deepEqual(events, [{ type: 'settingsChanged' }])
  const oauth = async (route: string, fields?: object) =>
    routes[fields ? `POST /api/chatgpt/${route}` : 'GET /api/chatgpt'](fields, new URL(`http://fixture/${route}`)) as Promise<any>
  const begin = await oauth('signin', {})
  assert.equal(begin.attemptId, 'attempt-a')
  assert.equal((await oauth('')).signingIn, true)
  assert.equal((await oauth('?attemptId=attempt-a')).signingIn, true)
  assert.equal(calls.at(-1)?.fields.attempt_id, 'attempt-a')
  host!.event({ type: 'SignInChanged', value: { profile: 'desktop' } })
  assert.equal(events.length, 1)
  host!.event({ type: 'SignInChanged', value: { profile: `web:${dir}` } })
  assert.deepEqual(events.at(-1), { type: 'chatgptChanged' })
  host!.event({ type: 'ProviderChanged', value: { profile: 'desktop' } })
  assert.deepEqual(events.at(-1), { type: 'providerChanged' })
  host!.event({ type: 'ProviderChanged', value: { profile: `web:${dir}` } })
  assert.deepEqual(events.at(-1), { type: 'chatgptChanged' })
  await oauth('cancel', { attemptId: begin.attemptId })
  const cancelled = await oauth('')
  assert.equal(cancelled.signingIn, false)
  assert.equal(cancelled.outcome?.ok, false)
  assert.deepEqual(await call('sessions', { path: folders[1] }), [])
  await call('search', { path: folders[1], text: 'Only A' })
  assert.equal(calls.find(c => c.method === 'ConversationSearchRequest')?.fields.project_id, 'project-b')
  const a = await call('conversation', { path: folders[0], session: 'a' })
  assert.deepEqual(a.permissions[0].asked, [asked, write, run])
  assert.deepEqual(a.permissions[0].asked.filter(projectAccessNeededNow), [asked])
  assert.equal(a.eventSeq, '0')
  assert.ok(!calls.some(c => c.method === 'ConversationActionRequest' || c.method === 'ConversationSendRequest'))
  await call('permission', { id: 'pending-a', allow: true, selected: [Access.Read, Access.Write] })
  assert.deepEqual(calls.filter(c => c.method === 'AccessResolveRequest').at(-1)?.fields,
    { request_id: 'pending-a', allow: true, selected: [Access.Read, Access.Write] })
  pendingAsked = [run.replace('"needed_now":false', '"needed_now":true')]
  const later = await call('conversation', { path: folders[0], session: 'a' })
  assert.deepEqual(later.permissions[0].asked, pendingAsked)
  await call('permission', { id: 'pending-a', allow: false, selected: [Access.Run] })
  assert.deepEqual(calls.filter(c => c.method === 'AccessResolveRequest').at(-1)?.fields,
    { request_id: 'pending-a', allow: false, selected: [Access.Run] })
  const b = await call('conversation', { path: folders[1], session: 'b' })
  assert.deepEqual(b.permissions, [])
  assert.equal(b.provider.model, 'gpt-6.1-sol')
  model = 'gpt-6.1-terra'
  host!.event({ type: 'SettingsChanged', value: { revision: 'host:3' } })
  await new Promise(resolve => setImmediate(resolve))
  assert.equal((await call('conversation', { path: folders[1], session: 'b' })).provider.model, model)
  busy = true
  model = 'gpt-6.1-sol'
  host!.event({ type: 'SettingsChanged', value: { revision: 'host:4' } })
  await new Promise(resolve => setImmediate(resolve))
  assert.equal((await call('conversation', { path: folders[1], session: 'b' })).provider.model, 'gpt-6.1-terra')
  await assert.rejects(call('conversation', { path: folders[1], session: 'a' }), /conflict/)
} finally {
  CoreHost.prototype.getSettings = original.settings
  CoreHost.prototype.provisionEnvironment = original.environment
  CoreHost.prototype.project = original.project
  fs.rmSync(dir, { recursive: true, force: true })
}

const choices = [asked, write, run]
const groups = [{ title: 'Collatz', path: folders[0], displayPath: folders[0], lines: choices.map(request => {
  const fields = JSON.parse(request.slice('project_access '.length))
  return { request, operation: fields.access, neededNow: fields.needed_now }
}) }]
const selections: string[] = []
const originalFetch = globalThis.fetch
const sandboxRequest = 'sandbox_off Collatz'
const sandboxLabel = 'Run outside the sandbox'
try {
  globalThis.fetch = async () => { throw new Error('Host unavailable at startup') }
  await permissionCatalog.load()
  let requested = false
  globalThis.fetch = async (url) => {
    assert.equal(url, '/api/permission-kinds')
    requested = true
    return Response.json({ kinds: [{ id: 'sandbox_off', label: sandboxLabel, icon: 'hand.raised' }] })
  }
  const props = { asked: [sandboxRequest], selected: [], onSelect() {} }
  const refreshed = Promise.withResolvers<void>()
  const unsubscribe = permissionCatalog.subscribe(refreshed.resolve)
  let prompt!: ReturnType<typeof create>
  act(() => { prompt = create(createElement(PermissionRequests, props)) })
  try {
    assert.ok(requested)
    await refreshed.promise
    assert.ok(prompt.root.findByType('input').props['aria-label'].includes(sandboxLabel))
  } finally { unsubscribe(); prompt.unmount() }
} finally { globalThis.fetch = originalFetch }
const view = create(createElement(PermissionRequests, {
  asked: choices, groups, selected: [asked], onSelect(request, on) { if (on) selections.push(request) },
}))
try {
  const group = view.root.findByType('li')
  assert.equal(group.findByType('strong').props.children, groups[0].title)
  const toggles = group.findAllByProps({ type: 'checkbox' })
  assert.equal(toggles.filter(toggle => toggle.props.checked).length, 1)
  for (const toggle of toggles) toggle.props.onChange({ target: { checked: true } })
  assert.deepEqual(new Set(selections), new Set(choices))
} finally { view.unmount() }
const label = toolStep('project_file_read', JSON.stringify({ card: ['analysis', 'result'] }), () => 'Analysis Result')
assert.equal(label.card, 'analysis/result')
assert.ok(label.running.includes('Analysis Result') && label.done.includes('Analysis Result'))
console.log('Agent real-check regressions: project isolation, permission labels, host model changes and tool labels passed')
