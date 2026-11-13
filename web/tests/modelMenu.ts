import { mockHostSettings } from './hostSettingsFixture'
import assert from 'node:assert/strict'
import { chatRoutes } from '../server/chat'
import { CoreAgentHost } from '../server/coreAgent'
import { ModelListRefresher } from '../src/model/modelLists'

const tick = () => new Promise((r) => setTimeout(r, 0))

// Each opening lists again; a failure is only that connection's; a slow
// connection holds nothing back and is not asked twice.
{
  const calls: string[] = []
  const slow: { release?: () => void } = {}
  const r = new ModelListRefresher((id) => {
    calls.push(id)
    if (id === 'broken') return Promise.reject(new Error('down'))
    if (id === 'slow') return new Promise<void>((resolve) => { slow.release = resolve })
    return Promise.resolve()
  })
  const finished: [string, boolean][] = []
  r.refresh(['fast', 'broken', 'slow'], (id, ok) => finished.push([id, ok]))
  await tick()
  assert.deepEqual(finished, [['fast', true], ['broken', false]])
  r.refresh(['fast', 'broken', 'slow'], (id, ok) => finished.push([id, ok]))
  await tick()
  assert.deepEqual(calls, ['fast', 'broken', 'slow', 'fast', 'broken'])
  slow.release!()
  await tick()
  assert.deepEqual(finished.at(-1), ['slow', true])
  r.refresh(['slow'], () => {})
  assert.equal(calls.at(-1), 'slow')
}

// The HTTP adapter shows the service's catalog and does not persist a second copy.
{
  process.env.ANTHROPIC_API_KEY = 'test-a'
  const files = new Map<string, unknown>()
  const original = CoreAgentHost.prototype.models
  const stored = CoreAgentHost.prototype.storedModels
  CoreAgentHost.prototype.models = async () => [{ id: 'a', name: 'Model A' }]
  CoreAgentHost.prototype.storedModels = async (id) => id === 'chatgpt' ? [{ id: 'g', name: 'GPT' }] : []
  try {
    const restoreSettings = mockHostSettings()
    const routes = chatRoutes({
      support: '/nonexistent', envs: {} as never, projectDir: (p) => String(p), broadcast: () => {}, here: '/nonexistent',
      httpError: (status, message) => Object.assign(new Error(message), { status }),
      readJSON: <T>(_file: string, fallback: T) => fallback,
      writeJSON: (file, value) => void files.set(file, value),
    })
    const result = await routes['POST /api/settings/models']({ provider: 'anthropic' }, new URL('http://x'))
    assert.deepEqual(result, { models: ['a'], modelNames: { g: 'GPT' } })
    assert.equal(files.size, 0)
    restoreSettings()
  } finally {
    CoreAgentHost.prototype.models = original
    CoreAgentHost.prototype.storedModels = stored
    delete process.env.ANTHROPIC_API_KEY
  }
}
console.log('model menu: passed')
