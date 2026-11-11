// What the Mac app does and the page must too: leaving a project closes what
// belongs to it, a project taken in keeps its binary files, the failure of a
// read of the versions is said, and helpers read the chat's attached folders.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { parse } from '../src/shared/json'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')
const { AppModel } = await import('../src/model/app')
const { Agent } = await import('../server/agent')

const calls: string[] = []
let answer: unknown = {}
const realFetch = globalThis.fetch
globalThis.fetch = (async (url: string) => {
  calls.push(String(url))
  return { ok: true, status: 200, json: async () => answer }
}) as typeof fetch

const session = (dir = '/p', key = 'k') => {
  const s = new ProjectSession(dir, key, 'p')
  s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  s.confirmedProject = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  s.layout = parse('{}')
  s.confirmedLayout = parse('{}')
  s.scheduleSave = () => {}
  return s
}

// Leaving: the project's windows and chat close; another project's and the app's stay.
{
  const app = new AppModel()
  const left = session('/a', 'a'), other = session('/b', 'b')
  app.sessions.set('a', left)
  app.sessions.set('b', other)
  const closed: string[] = []
  ;(app as any).chats.set('a', { close: () => closed.push('a') })
  ;(app as any).chats.set('b', { close: () => closed.push('b') })
  const window = (session: unknown, kind: string) => ({ id: kind, session, what: { kind }, x: 0, y: 0, width: 1, height: 1, z: 1 })
  app.windows = [window(left, 'card'), window(left, 'chat'), window(other, 'card'), window(undefined, 'appSettings')] as any
  app.closeProject(left)
  assert.deepEqual(app.windows.map((w) => [w.session === other, w.what.kind]), [[true, 'card'], [false, 'appSettings']])
  assert.deepEqual(closed, ['a'])
  assert.equal(app.sessions.has('a'), false)
  assert.equal(app.sessions.has('b'), true)

  // A session that was left does not start its environment again by reconnecting.
  calls.length = 0
  left.reconnect()
  await new Promise((r) => setTimeout(r, 20))
  assert.deepEqual(calls.filter((c) => c === '/api/env/start'), [])
}

// A project taken in: its binary files arrive byte for byte, as its text does.
{
  const bytes = Uint8Array.from({ length: 256 }, (_, i) => i)
  answer = {
    name: 'other',
    files: { 'tsugumi.json': '{"name":"o","canvas":{"cards":[],"connections":[]}}', 'definitions/x/note.txt': 'text' },
    binary: { 'definitions/x/blob.bin': Buffer.from(bytes).toString('base64') },
  }
  const s = session()
  s.connected = true
  await s.importProject('/o', [], { x: 0, y: 0 })
  assert.equal(s.sourceEdits.get('externals/other/definitions/x/note.txt'), 'text')
  assert.deepEqual(s.sourceEdits.get('externals/other/definitions/x/blob.bin'), bytes)
  assert.equal(await s.sourceText('externals/other/definitions/x/note.txt'), 'text')

  let sent: any[] = []
  const stopHere = new Error('sent')
  s.client = { save: async (_rev: unknown, _op: unknown, _origin: unknown, changes: any[]) => { sent = changes; throw stopHere } } as any
  await (s as any).saveOnce().catch(() => {})
  const put = sent.map((c) => c.value).find((v) => v?.path === 'externals/other/definitions/x/blob.bin')
  assert.ok(put, 'the binary file is saved')
  assert.deepEqual(put.bytes, Array.from(bytes))

  // Taking it back also takes the file back.
  s.undo()
  assert.equal(s.sourceEdits.has('externals/other/definitions/x/blob.bin'), false)
}

// The versions cannot be read: it is said, and the next read that works clears it.
{
  const s = session()
  s.client = { versionGraph: async () => { throw new Error('git failed') } } as any
  await s.refreshVersions()
  assert.match(s.versionGraphError ?? '', /git failed/)
  s.client = { versionGraph: async () => ({ enabled: false, commits: [] }) } as any
  await s.refreshVersions()
  assert.equal(s.versionGraphError, undefined)
}

// Helpers read the folders attached to the chat, as the Agent that called them does.
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-helpers-'))
  const a = Object.create(Agent.prototype) as any
  Object.assign(a, {
    folders: ['/attached'], storage: path.join(dir, 'c.json'), id: 'c', catalog: { tools: [] }, rpc: async () => ({}), provider: {}, references: {},
    conversation: { usage: { inputTokens: 0, outputTokens: 0 } }, save() {},
    readValue: async (tool: string) => (tool === 'project_inspect' ? { rev: 1, project: {}, definitions: [] } : { bytes: '' }),
  })
  const work = Agent.prototype.work
  const seen: string[][] = []
  Agent.prototype.work = async function (this: any) {
    seen.push(this.folders)
    return { outcome: 'finished', report: '', usage: { inputTokens: 0, outputTokens: 0 } }
  } as any
  try {
    const [, error] = await a.delegate(JSON.stringify({ tasks: [
      { name: 'one', cards: ['a'], instruction: 'Make the Card a from the attached folder.' },
      { name: 'two', cards: ['b'], instruction: 'Make the Card b from the attached folder.' },
    ] }), () => {})
    assert.equal(error, false)
    assert.deepEqual(seen, [['/attached'], ['/attached']])
  } finally {
    Agent.prototype.work = work
    fs.rmSync(dir, { recursive: true, force: true })
  }
}

globalThis.fetch = realFetch
console.log('leaving a project: passed')
