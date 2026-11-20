import { cardIds } from './cardIds'
// What the Mac app does and the page must too: leaving a project closes what
// belongs to it, a project taken in keeps its binary files, the failure of a
// read of the versions is said.
import assert from 'node:assert/strict'
import { parse } from '../shared/json'
import { R } from '../src/wire/rpc'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')
const { AppModel } = await import('../src/model/app')

const calls: string[] = []
let answer: unknown = {}
const realFetch = globalThis.fetch
globalThis.fetch = (async (url: string) => {
  calls.push(String(url))
  return { ok: true, status: 200, json: async () => answer }
}) as typeof fetch

const session = (dir = '/p', key = 'k') => {
  const s = cardIds(new ProjectSession(dir, key, 'p'))
  s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  s.confirmedProject = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  s.layout = parse('{}')
  s.confirmedLayout = parse('{}')
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
  left.environmentHost = { ...left.environmentHost, start: async () => {
    assert.fail('A closed session cannot restart its environment')
  } }
  left.reconnect()
  assert.equal(left.connected, false)
}

// The versions cannot be read: it is said, and the next read that works clears it.
{
  const s = session()
  s.client = {} as any
  s.projectHost = { ...s.projectHost, graph: async () => { throw Error('git failed') } }
  await s.refreshVersions()
  assert.match(s.versionGraphError ?? '', /git failed/)
  s.projectHost.graph = async () => Object.assign(new R.VersionGraphT(), { enabled: false })
  await s.refreshVersions()
  assert.equal(s.versionGraphError, undefined)
}

globalThis.fetch = realFetch
console.log('leaving a project: passed')
