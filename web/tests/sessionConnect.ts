// Reaching a tsugumid that has just started: a port that is not reachable at
// once (WSL2 forwards a new port to Windows a moment later) is tried again for
// a few seconds, and only a failure to reach it is told as one.
import assert from 'node:assert/strict'
import { RPCError } from '../src/shared/rpc'

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
} finally {
  globalThis.fetch = realFetch
}
console.log('session connect: passed')
