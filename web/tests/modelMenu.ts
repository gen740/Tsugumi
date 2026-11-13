import assert from 'node:assert/strict'
import { chatRoutes } from '../server/chat'
import { CoreAgentHost } from '../server/coreAgent'
import { keychainReader } from '../server/keychain'
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

// However often the menu opens, the secret is read (and the dialog can come)
// at most once: also after a refusal, until the item changes.
{
  let attrs = 'mdat 1', reads = 0, answer: { code: number; out: string } = { code: 0, out: 'key-1' }, there = true
  const k = keychainReader(async (_cmd, args) => {
    if (!args.includes('-w')) return { code: there ? 0 : 44, out: attrs }
    reads++
    return answer
  }, 'service')
  assert.deepEqual(await Promise.all([k.read('A'), k.read('A')]), ['key-1', 'key-1'])
  assert.equal(await k.read('A'), 'key-1')
  assert.equal(await k.exists('A'), true)
  assert.equal(reads, 1)
  attrs = 'mdat 2'
  answer = { code: 0, out: 'key-2' }
  assert.equal(await k.read('A'), 'key-2')
  assert.equal(reads, 2)
  // Refused: remembered, no second dialog.
  attrs = 'mdat 3'
  answer = { code: 128, out: '' }
  assert.equal(await k.read('A'), undefined)
  assert.equal(await k.read('A'), undefined)
  assert.equal(reads, 3)
  // Saved here within the same second: read again.
  k.forget('A')
  answer = { code: 0, out: 'key-3' }
  assert.equal(await k.read('A'), 'key-3')
  assert.equal(reads, 4)
  there = false
  assert.equal(await k.exists('A'), false)
  assert.equal(await k.read('A'), undefined)
  assert.equal(reads, 4)
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
    const routes = chatRoutes({
      support: '/nonexistent', envs: {} as never, projectDir: (p) => String(p), broadcast: () => {}, here: '/nonexistent',
      httpError: (status, message) => Object.assign(new Error(message), { status }),
      readJSON: <T>(_file: string, fallback: T) => fallback,
      writeJSON: (file, value) => void files.set(file, value),
    })
    const result = await routes['POST /api/settings/models']({ provider: 'anthropic' }, new URL('http://x'))
    assert.deepEqual(result, { models: ['a'], modelNames: { g: 'GPT' } })
    assert.equal(files.size, 0)
  } finally {
    CoreAgentHost.prototype.models = original
    CoreAgentHost.prototype.storedModels = stored
    delete process.env.ANTHROPIC_API_KEY
  }
}
console.log('model menu: passed')
