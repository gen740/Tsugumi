// What the session shows without having been told as it happened: the Cards of
// the last run when a project is opened, and a run while the tab is in the background.
import assert from 'node:assert/strict'
import { mock } from 'node:test'
import { createElement } from 'react'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import type { Workspace, WorkflowTab } from '../src/model/workspace'
import { parse } from '../shared/json'
import * as R from '../shared/gen/tsugumi/rpc'
import * as D from '../shared/gen/tsugumi/data'

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
  mock.timers.enable({ apis: ['setTimeout'] })
  try {
  const { s, inner } = session()
  const client: { onNotification?: (e: unknown) => void } = {}
  Object.assign(s, { client })
  inner.listen(client)
  client.onNotification?.({ subscriptionId: 'stale', seq: 1n })
  assert.equal(s.notificationCount, 0, 'batched, not at once')
  mock.timers.tick(250)
  assert.equal(s.notificationCount, 1, 'applied though no frame came')
  client.onNotification?.({ subscriptionId: 'stale', seq: 2n })
  Object.assign(s, { client: undefined })
  mock.timers.tick(250)
  assert.equal(s.notificationCount, 1, 'a closed connection cannot repopulate the session')
  } finally { mock.timers.reset() }
}

// Stop is shown during the request, clears on confirmation, and can be retried
// after failure. A late reply cannot change a finished or replacement run.
{
  const { s } = session()
  const status = (runId: string, state: D.RunState) =>
    Object.assign(new R.RunStatusT(), { runId, state })
  const sent: string[] = []
  let reject: (error: Error) => void = () => assert.fail('Stop was not sent')
  const rpc = {
    cancel(id: string): Promise<R.RunStatusT> {
      sent.push(id)
      return new Promise((_, fail) => { reject = fail })
    },
  }
  Object.assign(s, { client: rpc })
  s.runStatus = status('stop', D.RunState.preparing)
  const pending = s.cancelRun()
  assert.deepEqual(sent, ['stop'])
  assert.equal(s.stopRequested, true)
  assert.equal(s.canStopRun, false)
  await s.cancelRun()
  assert.deepEqual(sent, ['stop'])
  reject(Error('worker refused Stop'))
  await pending
  assert.match(s.stopError ?? '', /worker refused Stop/)
  assert.equal(s.canStopRun, true)

  const { CanvasRunControl } = await import('../src/ui/canvasWorkspace')
  const narrow = renderToStaticMarkup(createElement('div', { style: { width: 320 } },
    createElement(CanvasRunControl, { session: s, workspace: {} as Workspace, tab: { scope: [] } as WorkflowTab })))
  assert.match(narrow, /role="alert"[^>]*>[^<]*worker refused Stop/)

  rpc.cancel = async id => status(id, D.RunState.cancelling)
  await s.cancelRun()
  assert.equal(s.runStatus.state, D.RunState.cancelling)
  assert.equal(s.stopRequested, false)
  assert.equal(s.stopError, undefined)
  assert.equal(s.canStopRun, false)

  for (const next of [status('finished', D.RunState.succeeded), status('other', D.RunState.preparing)]) {
    s.runStatus = status('finished', D.RunState.running)
    rpc.cancel = async id => {
      s.runStatus = next
      return status(id, D.RunState.cancelling)
    }
    await s.cancelRun()
    assert.equal(s.runStatus, next)
    assert.equal(s.stopRequested, false)
  }
}

console.log('session restore: passed')
