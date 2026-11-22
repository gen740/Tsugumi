// Report connection failures after the host confirms project readiness.
import assert from 'node:assert/strict'
import { RPCError } from '../src/wire/rpc'
import { environmentState } from '../src/wire/environment'
import * as H from '../shared/gen/tsugumi/host'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')

type Inner = { openConnection(): Promise<void> }
const open = async (fail: () => Error) => {
  const s = new ProjectSession('/p', 'k', 'p')
  s.refreshProjectTrust = async () => {}
  s.environmentHost = { ...s.environmentHost,
    start: async () => ({ state: 'ready', endpoint: 'ws://127.0.0.1:1/rpc', token: 't', projectId: 'p' }),
  }
  let tries = 0
  s.openTransport = async () => {
    tries++
    throw fail()
  }
  await (s as unknown as Inner).openConnection()
  return { s, tries }
}

{
  // A transport failure is surfaced after the connection closes.
  {
    const { s, tries } = await open(() => new RPCError('disconnected', 'cannot connect to ws://127.0.0.1:1/rpc (1006)'))
    assert.equal(tries, 1)
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
  // A failed history open stays failed until the person retries explicitly.
  {
    const mismatch = { kind: 'incompatible_history', found: '2', expected: '3' }
    const session = new ProjectSession('/p', 'k', 'p')
    session.refreshProjectTrust = async () => {}
    session.environmentHost = { ...session.environmentHost, start: async () =>
      environmentState(Object.assign(new H.EnvironmentStateT(), {
        phase: H.EnvironmentPhase.Failed, startupError: Object.assign(new H.StartupErrorT(), mismatch),
      })),
    }
    session.openTransport = async () => { assert.fail('Failed environment cannot open a project transport') }
    await (session as unknown as Inner).openConnection()
    assert.equal(session.environment.state, 'failed')
    assert.equal(session.environment.historyVersionMismatch?.found, '2')
    session.dispose()
  }
}
console.log('session connect: passed')
