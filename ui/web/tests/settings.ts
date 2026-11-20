import assert from 'node:assert/strict'
import { chatRoutes, type ChatContext } from '../server/chat'
import { CoreHost } from '../server/coreHost'

// The Web adapter forwards one patch for the current surface and consumes
// the returned snapshot. Host RPC tests cover the ownership of each key.
const original = { settings: CoreHost.prototype.getSettings, environment: CoreHost.prototype.provisionEnvironment }
try {
  for (const desktop of [false, true]) {
    let reads = 0, current = 0
    let store = "first"
    const revision = (sequence = current) => `${store}:${sequence}`
    let host: CoreHost
    let reconnected: () => void
    const reloaded = new Promise<void>(resolve => { reconnected = resolve })
    const writes: any[] = []
    const values: Record<string, unknown> = { 'agent.model': 'old', 'app.language': 'ja' }
    const snapshot = () => ({ revision: revision(), values: Object.entries(values).map(([key, v]) => ({ key, value_json: JSON.stringify(v) })) })
    CoreHost.prototype.provisionEnvironment = async () => {}
    CoreHost.prototype.getSettings = async function (surface: string) {
      host = this
      reads++
      assert.equal(surface, 'web:/settings-test')
      this.user.call = async <T = any>(method: string, fields: any = {}): Promise<T> => {
        if (method === 'ModelSelectionRequest') return { provider: { providerId: 'anthropic', model: 'host-summary-model' } } as T
        if (method === 'ModelsListRequest') return [] as T
        if (method === 'ProviderSetRequest' || method === 'ProviderStatusRequest') return {} as T
        assert.equal(method, 'SettingsUpdateRequest')
        if (fields.revision !== revision()) throw new Error('conflict')
        writes.push(fields)
        for (const change of fields.changes) values[change.key] = JSON.parse(change.value_json)
        current++
        return snapshot() as T
      }
      return snapshot()
    }
    const routes = chatRoutes({ support: '/settings-test', here: '/fixture', desktop,
      envs: {} as never, projectDir: String, broadcast: (event: any) => {
        // The page's own reconnect notice comes from main.ts; chat routes
        // report the settings they read again.
        if (event.type === 'settingsChanged') reconnected()
      }, httpError: (_status, message) => new Error(message),
    } as ChatContext)
    const url = new URL('http://fixture')
    const skillFolders = [{ path: '/skills', enabled: true }]
    const result: any = await routes['POST /api/settings']({ revision: revision(0), model: 'new', skillFolders, vmCpus: 2 }, url)
    assert.equal(reads, 1)
    assert.equal(writes.length, 1)
    assert.equal(writes[0].profile, 'web:/settings-test')
    assert.deepEqual(Object.fromEntries(writes[0].changes.map((s: any) => [s.key, JSON.parse(s.value_json)])),
      { 'agent.model': 'new', 'agent.skillFolders': skillFolders, 'vm.cpus': 2 })
    assert.equal(result.model, 'new')
    assert.deepEqual(result.skillFolders, skillFolders)
    assert.equal(result.vmCpus, 2)
    assert.equal(result.revision, revision(1))
    await assert.rejects(routes['POST /api/settings']({ revision: revision(0), model: 'stale' }, url) as Promise<unknown>, /conflict/)
    assert.equal(values['agent.model'], 'new')
    // The folder editor still displays revision 1. Another tab removes the
    // folder, and this tab's unrelated settings request sees revision 2.
    await routes['POST /api/settings']({ revision: revision(1), skillFolders: [] }, url)
    assert.equal((await routes['GET /api/settings']({}, url) as any).revision, revision(2))
    await assert.rejects(routes['POST /api/settings']({
      revision: result.revision, skillFolders,
    }, url) as Promise<unknown>, /conflict/)
    assert.deepEqual(values['agent.skillFolders'], [])

    const stale = revision()
    // The RPC reconnects to a different store at the same sequence, before
    // the browser has refreshed the settings it is editing.
    store = "second"
    await assert.rejects(routes['POST /api/settings']({
      revision: stale, skillFolders,
    }, url) as Promise<unknown>, /conflict/)
    assert.deepEqual(values['agent.skillFolders'], [])
    values['agent.model'] = 'after-host-restart'
    host!.event({ type: 'Connected', value: {} })
    await reloaded
    const refreshed = await routes['GET /api/settings']({}, url) as any
    assert.equal(refreshed.model, 'after-host-restart')
    assert.equal(refreshed.revision, revision())
  }
} finally {
  CoreHost.prototype.getSettings = original.settings
  CoreHost.prototype.provisionEnvironment = original.environment
}
console.log('Host settings: combined patches and stale revisions passed')

// An editor may carry an older list than another settings panel. Preserve its
// explicit revision so hostd can reject it instead of resurrecting removed roots.
// A single-value write without one is checked against the host's current revision.
{
  const { api } = await import('../src/api')
  const originalFetch = globalThis.fetch
  const requests: any[] = []
  let reads = 0
  globalThis.fetch = (async (url, init) => {
    if (url !== "/api/settings") return new Response("{}")
    if (init?.body) requests.push(JSON.parse(String(init.body)))
    else reads++
    return new Response(JSON.stringify({ revision: 'host:0' }))
  }) as typeof fetch
  try {
    await api('/api/settings')
    await api('/api/settings', { model: 'from-revision-zero' })
    await api('/api/settings', { trustedFolders: ['/stale'], revision: 'host:3' })
    await api('/api/settings', { skillFolders: [], revision: 'host:2' })
    assert.equal(reads, 1)
    assert.equal(requests[0].revision, undefined)
    assert.equal(requests[1].revision, 'host:3')
    assert.equal(requests[2].revision, 'host:2')
  } finally { globalThis.fetch = originalFetch }
}
