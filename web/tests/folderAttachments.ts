import { mockHostSettings } from './hostSettingsFixture'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { ChatController } from '../src/model/chat'
import { chatRoutes } from '../server/chat'
import { CoreAgentHost } from '../server/coreAgent'

// One server request shares all selected folders before registering them. Equal
// names in different parent folders must keep separate resource identities.
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-folders-'))
const folders = ['first/data', 'second/data'].map(p => path.join(root, p))
for (const folder of folders) fs.mkdirSync(folder, { recursive: true })
const shares: string[][] = []
const registered: any[] = []
const originalProject = CoreAgentHost.prototype.project
let projects = 0
CoreAgentHost.prototype.project = async function () {
  projects++
  this.user.call = async (method: string, value: any) => {
    assert.equal(method, 'RegisterFolderRequest')
    registered.push(value)
    return {} as any
  }
  return 'project-id'
}
const restoreSettings = mockHostSettings()
try {
    const routes = chatRoutes({
    support: root, here: root, envs: { attachFolders: async (_p: string, _k: string, values: string[]) => { shares.push(values) } },
    projectDir: (p: unknown) => String(p),
    httpError: (_status: number, message: string) => new Error(message),
  } as any)
  const attach = routes['POST /api/chat/folders']
  const result = await attach({ path: root, selected: [...folders, folders[0]], folders: [] }) as any
  assert.deepEqual(shares, [folders])
  assert.equal(projects, 1)
  assert.deepEqual(registered.map(r => r.folder), folders)
  assert.equal(new Set(result.folders.map((r: any) => r.resource)).size, 2)
  assert.deepEqual(result.folders.map((r: any) => r.folder), folders)
  await assert.rejects(() => attach({ path: root, selected: [folders[0], path.join(root, 'missing')] }), /フォルダ/)
  assert.equal(shares.length, 1, 'An invalid batch does not share even its valid folders')

  // The client submits one batch, preserves both identities in the next message,
  // and leaves its attachment state unchanged after a failed operation.
  const c = Object.create(ChatController.prototype) as any
  Object.assign(c, { session: { dir: root }, currentId: 'conversation', folders: [], folderResources: new Map(),
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
  CoreAgentHost.prototype.project = originalProject
  fs.rmSync(root, { recursive: true, force: true })
}
console.log('multiple folder attachments: passed')
