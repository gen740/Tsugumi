import assert from 'node:assert/strict'
import { chatRoutes, type ChatContext } from '../src/host/chat'
import { CoreHost } from '../src/host/coreHost'

// Exercise the page's settings routes without a listener. The page and the
// desktop app share one profile; a patch writes only what the screen edited.
const original = { settings: CoreHost.prototype.getSettings }
const defaults = { 'agent.provider': 'anthropic', 'agent.model': 'old', 'agent.skillFolders': [], 'agent.trustedFolders': [], 'app.language': 'ja', 'agent.allowUnscopedHelpers': false }
try {
  const profiles = new Map<string, { revision: number; values: Record<string, unknown> }>()
  const writes: { profile: string; changes: { key: string; value_json: string }[] }[] = []
  const state = (profile: string) => {
    if (!profiles.has(profile)) profiles.set(profile, { revision: 0, values: { ...defaults } })
    return profiles.get(profile)!
  }
  CoreHost.prototype.getSettings = async function (profile: string) {
    this.user.call = async <T = any>(method: string, fields: any = {}): Promise<T> => {
      if (method === 'ModelSelectionRequest') return { provider: { providerId: 'anthropic', model: 'host-summary-model' } } as T
      if (method === 'ModelsListRequest') return [] as T
      assert.equal(method, 'SettingsUpdateRequest')
      const current = state(fields.profile)
      if (fields.revision !== current.revision) throw new Error('conflict')
      writes.push(fields)
      for (const change of fields.changes) current.values[change.key] = JSON.parse(change.value_json)
      current.revision++
      return {} as T
    }
    const current = state(profile)
    return { revision: current.revision, values: Object.entries(current.values).map(([key, v]) => ({ key, value_json: JSON.stringify(v) })) }
  }
  const routes = chatRoutes({ envs: {} as never, projectDir: String, broadcast: () => {},
    httpError: (_status, message) => new Error(message) } as ChatContext)
  const url = new URL('http://fixture')
  const revision = { desktop: 0 }
  const result: any = await routes['POST /api/settings']({ revision, model: 'new', allowUnscopedHelpers: true, skillFolders: [{ path: '/skills', enabled: true }], trustedFolders: ['/trials'] }, url)
  // One profile per host, shared with the desktop app: one revisioned write.
  assert.equal(writes.length, 1)
  assert.deepEqual([...profiles.keys()], ['desktop'])
  assert.deepEqual(state('desktop').values['agent.trustedFolders'], ['/trials'])
  assert.deepEqual(result.trustedFolders, ['/trials'])
  assert.equal(state('desktop').values['agent.model'], 'new')
  assert.equal(state('desktop').values['agent.allowUnscopedHelpers'], true)
  assert.deepEqual(state('desktop').values['agent.skillFolders'], [{ path: '/skills', enabled: true }])
  assert.equal(result.revision.desktop, 1)
  await assert.rejects(routes['POST /api/settings']({ revision, model: 'stale' }, url) as Promise<unknown>, /conflict/)
  assert.equal(state('desktop').values['agent.model'], 'new')
} finally {
  CoreHost.prototype.getSettings = original.settings
}
console.log('Host settings: one shared profile, combined patches and stale revisions passed')
