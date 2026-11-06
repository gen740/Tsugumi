import assert from 'node:assert/strict'
import { chatRoutes } from '../server/chat'
import { keychainReader } from '../server/keychain'
import { withModelList } from '../server/modelList'
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

// A list goes where Settings > API keeps it; an empty answer saves nothing.
{
  const s = { provider: 'openai', models: { anthropic: ['saved'] }, modelNames: { old: 'Old' } }
  assert.equal(withModelList(s, 'anthropic', []), null)
  assert.deepEqual(withModelList(s, 'openai', [{ id: 'o1', label: 'o1' }]),
    { provider: 'openai', models: { anthropic: ['saved'], openai: ['o1'] }, modelNames: { old: 'Old' } })
  assert.deepEqual(withModelList(s, 'chatgpt', [{ id: 'g', label: 'GPT' }])?.modelNames, { g: 'GPT' })
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
// Through the route: lists answered at the same time all stay, a failed or
// empty answer leaves what was saved.
{
  process.env.ANTHROPIC_API_KEY = 'test-a'
  process.env.OPENAI_API_KEY = 'test-o'
  const files = new Map<string, unknown>()
  const routes = chatRoutes({
    support: '/nonexistent', envs: {} as never, projectDir: (p) => String(p), broadcast: () => {}, here: '/nonexistent',
    httpError: (status, message) => Object.assign(new Error(message), { status }),
    readJSON: <T>(file: string, fallback: T) => (files.has(file) ? files.get(file) as T : fallback),
    writeJSON: (file, v) => void files.set(file, v),
  })
  const list = routes['POST /api/settings/models']
  const answers: Record<string, () => Response> = {}
  const original = globalThis.fetch
  globalThis.fetch = (async (_url: unknown, init?: RequestInit) => {
    await tick()
    const h = init?.headers as Record<string, string>
    return answers[h['x-api-key'] ? 'anthropic' : 'openai']()
  }) as typeof fetch
  try {
    const models = (...ids: string[]) => () => new Response(JSON.stringify({ data: ids.map((id) => ({ id })) }))
    answers.anthropic = models('a1', 'a2')
    answers.openai = models('o1')
    await Promise.all([list({ provider: 'anthropic' }, new URL('http://x')), list({ provider: 'openai' }, new URL('http://x'))])
    const saved = () => ([...files.values()][0] as { models: Record<string, string[]> }).models
    assert.deepEqual(saved(), { anthropic: ['a1', 'a2'], openai: ['o1'] })
    answers.anthropic = () => new Response('down', { status: 500 })
    answers.openai = models()
    await assert.rejects(async () => list({ provider: 'anthropic' }, new URL('http://x')))
    await assert.rejects(async () => list({ provider: 'openai' }, new URL('http://x')))
    assert.deepEqual(saved(), { anthropic: ['a1', 'a2'], openai: ['o1'] })
  } finally {
    globalThis.fetch = original
    delete process.env.ANTHROPIC_API_KEY
    delete process.env.OPENAI_API_KEY
  }
}
console.log('model menu: passed')
