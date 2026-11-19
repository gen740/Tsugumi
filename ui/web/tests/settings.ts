import assert from 'node:assert/strict'
import { chatRoutes, type ChatContext } from '../server/chat'
import { CoreHost } from '../server/coreHost'

// The Web adapter forwards one patch for the current surface and consumes
// the returned snapshot. Host RPC tests cover the ownership of each key.
const original = { settings: CoreHost.prototype.getSettings, environment: CoreHost.prototype.provisionEnvironment }
try {
  for (const desktop of [false, true]) {
    let reads = 0, current = 0
    const writes: any[] = []
    const values: Record<string, unknown> = { 'agent.model': 'old', 'app.language': 'ja' }
    const snapshot = () => ({ revision: current, values: Object.entries(values).map(([key, v]) => ({ key, value_json: JSON.stringify(v) })) })
    CoreHost.prototype.provisionEnvironment = async () => {}
    CoreHost.prototype.getSettings = async function (surface: string) {
      reads++
      assert.equal(surface, 'web:/settings-test')
      this.user.call = async <T = any>(method: string, fields: any = {}): Promise<T> => {
        if (method === 'ModelSelectionRequest') return { provider: { providerId: 'anthropic', model: 'host-summary-model' } } as T
        if (method === 'ModelsListRequest') return [] as T
        assert.equal(method, 'SettingsUpdateRequest')
        if (fields.revision !== current) throw new Error('conflict')
        writes.push(fields)
        for (const change of fields.changes) values[change.key] = JSON.parse(change.value_json)
        current++
        return snapshot() as T
      }
      return snapshot()
    }
    const routes = chatRoutes({ support: '/settings-test', here: '/fixture', desktop,
      envs: {} as never, projectDir: String, broadcast: () => {}, httpError: (_status, message) => new Error(message),
    } as ChatContext)
    const url = new URL('http://fixture')
    const skillFolders = [{ path: '/skills', enabled: true }]
    const result: any = await routes['POST /api/settings']({ revision: 0, model: 'new', skillFolders, vmCpus: 2 }, url)
    assert.equal(reads, 1)
    assert.equal(writes.length, 1)
    assert.equal(writes[0].profile, 'web:/settings-test')
    assert.deepEqual(Object.fromEntries(writes[0].changes.map((s: any) => [s.key, JSON.parse(s.value_json)])),
      { 'agent.model': 'new', 'agent.skillFolders': skillFolders, 'vm.cpus': 2 })
    assert.equal(result.model, 'new')
    assert.deepEqual(result.skillFolders, skillFolders)
    assert.equal(result.vmCpus, 2)
    assert.equal(result.revision, 1)
    await assert.rejects(routes['POST /api/settings']({ revision: 0, model: 'stale' }, url) as Promise<unknown>, /conflict/)
    assert.equal(values['agent.model'], 'new')
  }
} finally {
  CoreHost.prototype.getSettings = original.settings
  CoreHost.prototype.provisionEnvironment = original.environment
}
console.log('Host settings: combined patches and stale revisions passed')

// An editor may carry an older list than another settings panel. Preserve its
// explicit revision so hostd can reject it instead of resurrecting removed roots.
{
  const { api } = await import('../src/api')
  const originalFetch = globalThis.fetch
  const requests: any[] = []
  let reads = 0
  globalThis.fetch = (async (url, init) => {
    if (url !== "/api/settings") return new Response("{}")
    if (init?.body) requests.push(JSON.parse(String(init.body)))
    else reads++
    return new Response(JSON.stringify({ revision: reads - 1 }))
  }) as typeof fetch
  try {
    await api('/api/settings')
    await api('/api/settings', { model: 'from-revision-zero' })
    await api('/api/settings', { trustedFolders: ['/stale'], revision: 3 })
    assert.equal(reads, 1)
    assert.equal(requests[0].revision, 0)
    assert.equal(requests[1].revision, 3)
  } finally { globalThis.fetch = originalFetch }
}

// Permission settings forward each edit to the host and display its returned list.
{
  const original = { settings: CoreHost.prototype.getSettings, environment: CoreHost.prototype.provisionEnvironment }
  const calls: { method: string; fields: any }[] = []
  const snapshot = { projects: [{ project_id: 'project', title: 'Project', path: '/project', permissions: [] }] }
  try {
    CoreHost.prototype.provisionEnvironment = async () => {}
    CoreHost.prototype.getSettings = async function () {
      this.user.call = async <T = any>(method: string, fields: any = {}): Promise<T> => {
        calls.push({ method, fields })
        return snapshot as T
      }
      return { revision: 0, values: [] }
    }
    const routes = chatRoutes({ support: '/permission-settings', here: '/fixture', envs: {} as never,
      projectDir: String, broadcast: () => {}, httpError: (_status, message) => new Error(message) })
    const url = new URL('http://fixture')
    assert.equal(await routes['POST /api/project-permissions']({}, url), snapshot)
    const edit = { project_id: 'project', resource_id: 'folder', access: 'Read', allow: true }
    assert.equal(await routes['POST /api/project-permissions/set'](edit, url), snapshot)
    assert.deepEqual(calls, [{ method: 'ProjectPermissionsRequest', fields: {} },
      { method: 'ProjectPermissionSetRequest', fields: edit }])
  } finally {
    CoreHost.prototype.getSettings = original.settings
    CoreHost.prototype.provisionEnvironment = original.environment
  }
}
