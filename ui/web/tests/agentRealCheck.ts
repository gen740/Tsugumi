import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { chatRoutes, cancelChats, type ChatContext } from '../server/chat'
import { CoreHost } from '../server/coreHost'
import { toolStep, PermissionRequests } from '../src/ui/chat'
import { AGENT, projectAccessNeededNow } from '../shared/agentShared'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { createElement } from 'react'
import EN from '../shared/i18n/en.json'

// The HTTP adapter must scope lists/search and adopt host edits and the
// accepted turn's model. No socket, model or environment is used here.
const dir = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-host-copies-')))
const folders = ['a', 'b'].map(n => path.join(dir, n))
for (const folder of folders) {
  fs.mkdirSync(folder)
  fs.writeFileSync(path.join(folder, 'tsugumi.json'), '{"name":"Collatz"}')
}
const original = { settings: CoreHost.prototype.getSettings, environment: CoreHost.prototype.provisionEnvironment,
  project: CoreHost.prototype.project, folder: CoreHost.prototype.projectFolder }
let model = 'gpt-6.1-sol'
let busy = false
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
  CoreHost.prototype.projectFolder = () => undefined
  CoreHost.prototype.getSettings = async function() {
    host = this
    this.user.call = async <T>(method: string, fields: any = {}): Promise<T> => {
      calls.push({ method, fields })
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
      if (method === 'EventsRequest') return { events: [], next_seq: 0n, busy } as T
      if (method === 'AccessListRequest') return { requests: (!fields.conversation_id || fields.conversation_id === 'a') ? [{ id: 'pending-a', state: 'pending', asked: pendingAsked }] : [] } as T
      throw new Error(`Unexpected call: ${method}`)
    }
    return { revision: 1, values: Object.entries({ 'agent.provider': 'chatgpt', 'agent.model.chatgpt': model,
      'agent.effort': '', 'agent.fastModel': '', 'agent.skillFolders': [], 'app.language': 'en' })
      .map(([key, value]) => ({ key, value_json: JSON.stringify(value) })) }
  }
  const routes = chatRoutes({ support: dir, here: dir, desktop: true, envs: {} as never, projectDir: String,
    listeners: () => 0, broadcast(event) { events.push(event) }, httpError: (_status, message) => new Error(message) } as ChatContext)
  const call = async (route: string, fields: any) => routes[`POST /api/chat/${route}`](fields, new URL('http://fixture')) as Promise<any>
  assert.deepEqual(await call('sessions', { path: folders[0] }), [{ id: 'a', title: 'Only A' }])
  host!.event({ type: 'SettingsChanged', value: { profile: 'desktop', revision: 2 } })
  await new Promise(resolve => setImmediate(resolve))
  assert.deepEqual(events, [{ type: 'settingsChanged' }])
  host!.event({ type: 'SettingsChanged', value: { profile: 'another-profile', revision: 2 } })
  assert.equal(events.length, 1, 'unrelated profiles do not refresh this client')
  assert.deepEqual(await call('sessions', { path: folders[1] }), [])
  await call('search', { path: folders[1], text: 'Only A' })
  assert.equal(calls.find(c => c.method === 'ConversationSearchRequest')?.fields.project_id, 'project-b')
  const a = await call('conversation', { path: folders[0], session: 'a' })
  assert.deepEqual(a.permission.asked, [asked, write, run])
  assert.deepEqual(a.permission.asked.filter(projectAccessNeededNow), [asked])
  await call('permission', { id: 'pending-a', allow: true, selected: [asked, write] })
  assert.deepEqual(calls.filter(c => c.method === 'AccessResolveRequest').at(-1)?.fields,
    { request_id: 'pending-a', allow: true, selected: [asked, write] })
  pendingAsked = [run.replace('"needed_now":false', '"needed_now":true')]
  const later = await call('conversation', { path: folders[0], session: 'a' })
  assert.deepEqual(later.permission.asked, pendingAsked)
  await call('permission', { id: 'pending-a', allow: false, selected: pendingAsked })
  assert.deepEqual(calls.filter(c => c.method === 'AccessResolveRequest').at(-1)?.fields,
    { request_id: 'pending-a', allow: false, selected: pendingAsked })
  const b = await call('conversation', { path: folders[1], session: 'b' })
  assert.equal(b.permission, null)
  assert.equal(b.provider.model, 'gpt-6.1-sol')
  model = 'gpt-6.1-terra'
  host!.event({ type: 'SettingsChanged', value: { profile: 'desktop', revision: 3 } })
  await new Promise(resolve => setImmediate(resolve))
  assert.equal((await call('conversation', { path: folders[1], session: 'b' })).provider.model, model)
  busy = true
  model = 'gpt-6.1-sol'
  host!.event({ type: 'SettingsChanged', value: { profile: 'desktop', revision: 4 } })
  await new Promise(resolve => setImmediate(resolve))
  assert.equal((await call('conversation', { path: folders[1], session: 'b' })).provider.model, 'gpt-6.1-terra')
  await assert.rejects(call('conversation', { path: folders[1], session: 'a' }), /conflict/)
} finally {
  for (const folder of folders) cancelChats(folder)
  CoreHost.prototype.getSettings = original.settings
  CoreHost.prototype.provisionEnvironment = original.environment
  CoreHost.prototype.project = original.project
  CoreHost.prototype.projectFolder = original.folder
  fs.rmSync(dir, { recursive: true, force: true })
}

const tools = [...AGENT.requests, ...AGENT.approvalRequests].map(name => name.replace(/Request$/, '').replace(/[A-Z]/g, (c, index) => (index ? '_' : '') + c.toLowerCase()))
for (const lang of ['ja', 'en'] as const) {
  const choices = [asked, write, run]
  const groups = [{ title: 'Collatz', path: folders[0], displayPath: folders[0], lines: choices.map(request => {
    const fields = JSON.parse(request.slice('project_access '.length))
    return { request, operation: fields.access, neededNow: fields.needed_now }
  }) }]
  const markup = renderToStaticMarkup(createElement(PermissionRequests, { asked: choices, groups, selected: [asked], onSelect() {} }))
  assert.equal((markup.match(/Collatz/g) ?? []).length, 1, 'The target is drawn once for all operations')
  assert.equal((markup.match(/type="checkbox"/g) ?? []).length, choices.length)
  for (const name of [...tools, 'demo_list', 'demo_read', 'operation_approval', 'guide_show', 'delegate', 'web_search']) {
    const label = toolStep(name, '{}')
    assert.ok(label.running && label.done)
    if (lang === 'en') {
      assert.ok((EN as Record<string, string>)[label.done], `Missing English label: ${label.done}`)
      assert.ok((EN as Record<string, string>)[label.running.slice(0, -1)], `Missing English label: ${label.running}`)
    }
  }
}
console.log('Agent real-check regressions: project isolation, permission labels, host model changes and tool labels passed')
