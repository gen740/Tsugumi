import { mockHostSettings } from './hostSettingsFixture'
import assert from 'node:assert/strict'
import { chatRoutes } from '../server/chat'
import { CoreHost } from '../server/coreHost'
import { ModelListRefresher } from '../src/model/modelLists'

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
  const pending = new Map<number, () => void>()
  const waitFor = (n: number) => new Promise<void>(resolve => pending.set(n, resolve))
  const record = (id: string, ok: boolean) => {
    finished.push([id, ok]); pending.get(finished.length)?.()
  }
  const first = waitFor(2)
  r.refresh(['fast', 'broken', 'slow'], record)
  await first
  assert.deepEqual(new Map(finished), new Map([['fast', true], ['broken', false]]))
  const again = waitFor(4)
  r.refresh(['fast', 'broken', 'slow'], record)
  await again
  assert.deepEqual(calls.toSorted(), ['broken', 'broken', 'fast', 'fast', 'slow'])
  const last = waitFor(5)
  slow.release!()
  await last
  assert.deepEqual(finished.at(-1), ['slow', true])
  r.refresh(['slow'], () => {})
  assert.equal(calls.filter(id => id === 'slow').length, 2)
}

// The HTTP adapter shows the service's catalog and does not persist a second copy.
{
  process.env.ANTHROPIC_API_KEY = 'test-a'
  const original = CoreHost.prototype.models
  const stored = CoreHost.prototype.storedModels
  CoreHost.prototype.models = async () => [{ id: 'a', name: 'Model A' }]
  CoreHost.prototype.storedModels = async (id) => id === 'chatgpt' ? [{ id: 'g', name: 'GPT' }] : []
  try {
    const restoreSettings = mockHostSettings()
    const routes = chatRoutes({
      support: '/nonexistent', envs: {} as never, projectDir: (p) => String(p), broadcast: () => {}, here: '/nonexistent',
      httpError: (status, message) => Object.assign(new Error(message), { status }),
    })
    const result = await routes['POST /api/settings/models']({ provider: 'anthropic' }, new URL('http://x'))
    assert.deepEqual(result, { models: ['a'], modelNames: { g: 'GPT' } })
    restoreSettings()
  } finally {
    CoreHost.prototype.models = original
    CoreHost.prototype.storedModels = stored
    delete process.env.ANTHROPIC_API_KEY
  }
}
console.log('model menu: passed')
