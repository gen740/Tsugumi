// What the session shows without having been told as it happened: the Cards of
// the last run when a project is opened, and a run while the tab is in the background.
import assert from 'node:assert/strict'
import { parse } from '../src/shared/json'
import * as R from '../src/gen/tsugumi/rpc'
import * as D from '../src/gen/tsugumi/data'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
// A tab in the background: frames never come.
page.requestAnimationFrame = () => 1
page.cancelAnimationFrame = () => {}
const { ProjectSession } = await import('../src/model/session')

type Inner = {
  subscription: string
  lastSeq: bigint
  listen(c: unknown): void
  restoreCardStates(c: unknown): Promise<void>
}
const session = () => {
  const s = new ProjectSession('/p', 'k', 'p')
  s.project = parse('{"name":"t","canvas":{"cards":[{"id":"a"}],"connections":[]}}')
  s.layout = parse('{}')
  return { s, inner: s as unknown as Inner }
}
const invocation = (id: string, state: D.CardState, n = 1n) =>
  Object.assign(new R.InvocationSummaryT(), { card: Object.assign(new D.CardPathT(), { segments: [id] }), invocation: n, state })

// Opened after the run: its Cards come from the record.
{
  const { s, inner } = session()
  s.runStatus = Object.assign(new R.RunStatusT(), { runId: 'r1', state: D.RunState.failed })
  const asked: string[] = []
  await inner.restoreCardStates({
    latestInvocations: async (id: string) => {
      asked.push(id)
      return { records: [invocation('a', D.CardState.failed, 2n), invocation('b', D.CardState.blocked)], batches: [] }
    },
  })
  assert.deepEqual(asked, ['r1'])
  assert.equal(s.cardStates.get('a')?.state, D.CardState.failed)
  assert.equal(s.cardStates.get('a')?.invocation, 2n)
  assert.equal(s.noResultNote('a'), '失敗したので結果はありません')
  assert.equal(s.noResultNote('b'), '上流の Card が失敗したので実行していません')
  assert.equal(s.noResultNote('c'), undefined, 'not run: nothing to explain')
}

// A run that started while it asked keeps its own states.
{
  const { s, inner } = session()
  s.runStatus = Object.assign(new R.RunStatusT(), { runId: 'r1' })
  await inner.restoreCardStates({
    latestInvocations: async () => {
      s.runStatus = Object.assign(new R.RunStatusT(), { runId: 'r2' })
      return { records: [invocation('a', D.CardState.succeeded)], batches: [] }
    },
  })
  assert.equal(s.cardStates.size, 0)
}

// No run yet: nothing to ask.
{
  const { inner } = session()
  await inner.restoreCardStates({ latestInvocations: async () => assert.fail('no run to ask about') })
}

// In the background a notification is still applied, without a frame.
{
  const { s, inner } = session()
  const client: { onNotification?: (e: unknown) => void } = {}
  inner.listen(client)
  client.onNotification?.({ subscriptionId: 'stale', seq: 1n })
  assert.equal(s.notificationCount, 0, 'batched, not at once')
  await new Promise((r) => setTimeout(r, 350))
  assert.equal(s.notificationCount, 1, 'applied though no frame came')
}

console.log('session restore: passed')
