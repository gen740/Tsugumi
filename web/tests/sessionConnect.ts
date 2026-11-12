// Reaching a tsugumid that has just started: a port that is not reachable at
// once (WSL2 forwards a new port to Windows a moment later) is tried again for
// a few seconds, and only a failure to reach it is told as one.
import assert from 'node:assert/strict'
import { RPCError } from '../src/shared/rpc'
import { historyVersionMismatch } from '../src/shared/historyVersion'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')

// The host says the environment is ready.
const realFetch = globalThis.fetch
globalThis.fetch = (async () => new Response(JSON.stringify({ state: 'ready', endpoint: 'ws://127.0.0.1:1/rpc', token: 't', projectId: 'p' }))) as typeof fetch

type Inner = { openConnection(): Promise<void> }
const open = async (fail: () => Error) => {
  const s = new ProjectSession('/p', 'k', 'p')
  s.connectRetryDelays = [1, 1, 1]
  let tries = 0
  s.openTransport = async () => {
    tries++
    throw fail()
  }
  await (s as unknown as Inner).openConnection()
  return { s, tries }
}

try {
  // Not reached: tried once and then once after each wait, then told as a connection failure.
  {
    const { s, tries } = await open(() => new RPCError('disconnected', 'cannot connect to ws://127.0.0.1:1/rpc (1006)'))
    assert.equal(tries, 4)
    assert.equal(s.connected, false)
    assert.equal(s.connectionFailed, true)
    assert.match(s.connectionMessage ?? '', /cannot connect/)
  }
  // Another failure is not tried again, and is not told as a connection failure.
  {
    const { s, tries } = await open(() => new RPCError('protocol', 'bad hello'))
    assert.equal(tries, 1)
    assert.equal(s.connectionFailed, false)
    assert.match(s.connectionMessage ?? '', /bad hello/)
  }
  // Retrying never opts in to archiving. Only a typed mismatch exposes the
  // separate action, and a second click while it starts has no effect.
  {
    const mismatch = { kind: 'incompatible_history', found: '2', expected: '3' }
    assert.deepEqual(historyVersionMismatch(mismatch), mismatch)
    assert.equal(historyVersionMismatch({ ...mismatch, kind: 'database_corrupt' }), undefined)
    assert.equal(historyVersionMismatch({ ...mismatch, found: 2 }), undefined)
    const routes: string[] = []
    globalThis.fetch = (async (url) => {
      routes.push(String(url))
      return new Response(JSON.stringify({ state: 'failed', historyVersionMismatch: mismatch }))
    }) as typeof fetch
    const session = new ProjectSession('/p', 'k', 'p')
    session.archiveHistoryAndStart()
    assert.deepEqual(routes, [])
    await (session as unknown as Inner).openConnection()
    assert.equal(session.environment.state, 'failed')
    session.archiveHistoryAndStart()
    session.archiveHistoryAndStart()
    assert.deepEqual(routes, ['/api/env/start', '/api/env/archive-history'])
    session.dispose()
  }
} finally {
  globalThis.fetch = realFetch
}
console.log('session connect: passed')
