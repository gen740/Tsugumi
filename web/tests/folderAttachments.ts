import { mockHostSettings } from './hostSettingsFixture'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { ChatController } from '../src/model/chat'
import { chatRoutes } from '../server/chat'
import { CoreHost } from '../server/coreHost'

// One server request shares all selected folders before registering them. Equal
// names in different parent folders must keep separate resource identities.
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-folders-'))
const folders = ['first/data', 'second/data'].map(p => path.join(root, p))
for (const folder of folders) fs.mkdirSync(folder, { recursive: true })
const shares: string[][] = []
const originalProject = CoreHost.prototype.project
CoreHost.prototype.project = async function () { throw new Error('Attach must not look up the old core') }
const restoreSettings = mockHostSettings()
try {
    const routes = chatRoutes({
    support: root, here: root, envs: { attachFolders: async (_p: string, values: string[]) => {
      for (const folder of values) if (!fs.existsSync(folder)) throw new Error('Folder not found')
      const unique = [...new Set(values)]
      shares.push(unique)
      return unique.map((folder, i) => ({ folder, resource: `folder:project:${i}` }))
    } },
    projectDir: (p: unknown) => String(p),
    httpError: (_status: number, message: string) => new Error(message),
  } as any)
  const attach = routes['POST /api/chat/folders']
  const result = await attach({ path: root, selected: [...folders, folders[0]], folders: [] }) as any
  assert.deepEqual(shares, [folders])
  assert.equal(new Set(result.folders.map((r: any) => r.resource)).size, 2)
  assert.deepEqual(result.folders.map((r: any) => r.folder), folders)
  await assert.rejects(() => attach({ path: root, selected: [folders[0], path.join(root, 'missing')] }), /Folder/)
  assert.equal(shares.length, 1, 'An invalid batch does not share even its valid folders')

  // The client submits one batch, preserves both identities in the next message,
  // and leaves its attachment state unchanged after a failed operation.
  const c = Object.create(ChatController.prototype) as any
  Object.assign(c, { session: { dir: root, withEnvironmentChange: async (run: () => Promise<unknown>) => run() }, currentId: 'conversation', displayedId: 'conversation', contentRevision: 0, folders: [], folderResources: new Map(),
    references: [], skills: [], items: [], available: true, busy: true, changed() {}, begin() {} })
  const originalFetch = globalThis.fetch
  const requests: { endpoint: string; body: any }[] = []
  try {
    globalThis.fetch = async (endpoint, init) => {
      requests.push({ endpoint: String(endpoint), body: JSON.parse(String(init?.body)) })
      return { ok: true, json: async () => result } as Response
    }
    await c.attach([...folders, folders[0]])
    assert.equal(requests.length, 1)
    assert.equal(requests[0].endpoint, '/api/chat/folders')
    assert.deepEqual(requests[0].body.selected, folders)
    assert.deepEqual(c.folders, folders)
    let sent: any
    c.request = async (_endpoint: string, body: unknown) => { sent = body }
    c.send('Read both folders')
    assert.deepEqual(sent.attachedResources, result.folders.map((r: any) => r.resource))
    globalThis.fetch = async () => ({ ok: false, json: async () => ({ error: 'Cannot attach' }) }) as Response
    await assert.rejects(() => c.attach(['/unavailable']), /Cannot attach/)
    assert.deepEqual(c.folders, folders)
  } finally { globalThis.fetch = originalFetch }
} finally {
  restoreSettings()
  CoreHost.prototype.project = originalProject
  fs.rmSync(root, { recursive: true, force: true })
}
// The attachment result is not exposed until this view has a new connection.
// Two chats sharing one project cannot close each other's replacement socket.
const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')
const session = Object.create(ProjectSession.prototype) as any
const order: string[] = []
let release!: () => void
const held = new Promise<void>(resolve => { release = resolve })
Object.assign(session, { connected: true, closed: false, changed() {},
  client: { close() { order.push('close') } },
  async openConnection() { order.push('connect'); this.connected = true; this.client = { close() { order.push('close') } } },
})
const first = session.withEnvironmentChange(async () => {
  assert.equal(session.connected, false)
  order.push('first')
  await held
  return 'registered'
})
let returned = false
void first.then(() => { returned = true })
const second = session.withEnvironmentChange(async () => {
  assert.equal(session.connected, false)
  order.push('second')
  return 'registered-again'
})
await Promise.resolve()
assert.equal(returned, false)
assert.deepEqual(order, ['close', 'first'])
release()
assert.deepEqual(await Promise.all([first, second]), ['registered', 'registered-again'])
assert.deepEqual(order, ['close', 'first', 'connect', 'close', 'second', 'connect'])
assert.equal(session.connected, true)
await assert.rejects(session.withEnvironmentChange(async () => { throw new Error('Host restored old shares') }), /Host restored/)
assert.equal(session.connected, true, 'A host failure reconnects the view to the restored environment')

// A ready host operation can outlive one transient socket failure. Once the
// view recovers, its successful registration must reach the chat as success.
let connections = 0
session.openConnection = async function () { this.connected = ++connections > 1 }
assert.equal(await session.withEnvironmentChange(async () => 'registered-after-restart'), 'registered-after-restart')
assert.equal(connections, 2)
assert.equal(session.connected, true)

console.log('multiple folder attachments: passed')
