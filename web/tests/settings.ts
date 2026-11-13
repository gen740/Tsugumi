import assert from 'node:assert/strict'
import { chatRoutes, type ChatContext } from '../server/chat'
import { CoreAgentHost } from '../server/coreAgent'

// Exercise the browser -> Web host revision boundary without a listener.
// Standalone Web uses one profile; desktop Web shares connection settings but
// retains its own skill folders. Both must patch only what the screen edited.
const original = { settings: CoreAgentHost.prototype.getSettings, environment: CoreAgentHost.prototype.provisionEnvironment }
const defaults = { 'agent.provider': 'anthropic', 'agent.model': 'old', 'agent.skillFolders': [], 'app.language': 'ja' }
try {
  for (const desktop of [false, true]) {
    const profiles = new Map<string, { revision: number; values: Record<string, unknown> }>()
    const writes: { profile: string; changes: { key: string; value_json: string }[] }[] = []
    const state = (profile: string) => {
      if (!profiles.has(profile)) profiles.set(profile, { revision: 0, values: { ...defaults } })
      return profiles.get(profile)!
    }
    CoreAgentHost.prototype.provisionEnvironment = async () => {}
    CoreAgentHost.prototype.getSettings = async function (profile: string) {
      this.user.call = async <T = any>(method: string, fields: any = {}): Promise<T> => {
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
    const routes = chatRoutes({ support: '/settings-test', here: '/fixture', desktop,
      envs: {} as never, projectDir: String, broadcast: () => {}, httpError: (_status, message) => new Error(message),
    } as ChatContext)
    const url = new URL('http://fixture')
    const connection = desktop ? 'desktop' : 'web:/settings-test'
    const revision = { [connection]: 0, 'web:/settings-test': 0 }
    const result: any = await routes['POST /api/settings']({ revision, model: 'new', skillFolders: [{ path: '/skills', enabled: true }] }, url)
    assert.equal(writes.length, desktop ? 2 : 1)
    assert.equal(state(connection).values['agent.model'], 'new')
    assert.deepEqual(state('web:/settings-test').values['agent.skillFolders'], [{ path: '/skills', enabled: true }])
    if (desktop) assert.deepEqual(state('desktop').values['agent.skillFolders'], [])
    assert.equal(result.revision[connection], 1)
    await assert.rejects(routes['POST /api/settings']({ revision, model: 'stale' }, url) as Promise<unknown>, /conflict/)
    assert.equal(state(connection).values['agent.model'], 'new')
  }
} finally {
  CoreAgentHost.prototype.getSettings = original.settings
  CoreAgentHost.prototype.provisionEnvironment = original.environment
}
console.log('Host settings: profile sharing, combined patches and stale revisions passed')
