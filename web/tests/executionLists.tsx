import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { parse } from '../src/shared/json'
import { D, R } from '../src/shared/rpc'
import { Scene } from '../src/model/canvas'
import { ProjectSession } from '../src/model/session'
import { Workspace } from '../src/model/workspace'
import { CardGeometry } from '../src/model/canvasKit'
import { WireLayer } from '../src/ui/cardNode'

const session = new ProjectSession('/p', 'p', 'p')
const doc = '{"canvas":{"cards":[{"id":"source"},{"id":"collect"},{"id":"sink"}],"connections":[{"from":{"card":"source","port":"out"},"to":{"card":"collect","port":"in"}},{"from":{"card":"collect","port":"out"},"to":{"card":"sink","port":"in"}}]}}'
session.project = parse(doc)
session.confirmedProject = parse(doc)
session.layout = parse('{}')
session.confirmedLayout = parse('{}')
session.confirmedDefinitions.set('source', parse('{"language":"bash"}'))
session.confirmedDefinitions.set('collect', parse('{"template":"collect"}'))
session.confirmedDefinitions.set('sink', parse('{"language":"bash"}'))
const ordinaryList = { name: 'out', kind: D.PortKind.FlatBuffer, rootType: 'Example.List', optional: false, executionShape: D.ExecutionShape.single }
session.resolution.set('source', { path: ['source'], inputs: [], outputs: [ordinaryList], diagnostics: [], nixReferences: [] })
session.resolution.set('collect', { path: ['collect'], inputs: [], outputs: [ordinaryList], diagnostics: [], nixReferences: [] })

assert.deepEqual(session.ports('collect'), { inputs: ['in'], outputs: ['out'] })
assert.equal(session.kind('collect'), 'collect')
for (const flow of ['topToBottom', 'leftToRight'] as const) {
  const geometry = new CardGeometry(session.nodeData('collect'), flow)
  assert.equal(geometry.shape, 'collect')
  const input = geometry.port('in', false), output = geometry.port('out', true)
  assert.ok(flow === 'topToBottom' ? input.x === output.x && input.y < output.y : input.y === output.y && input.x < output.x)
}
assert.equal(new Scene(session, []).wires[0].executionList, false, 'an ordinary FBS vector is a single value')

session.executionRev = 7n
session.runStatus = Object.assign(new R.RunStatusT(), { runId: 'run', executionRev: 7n, state: D.RunState.running })
const listener = session as unknown as { applyRunEvent(event: R.RunEventT): void }
function batch(activation: bigint, count: number, complete: boolean, failed = false, runId = 'run') {
  const changed = Object.assign(new R.RunBatchChangedT(), {
    batch: Object.assign(new R.RunBatchT(), {
      card: Object.assign(new D.CardPathT(), { segments: ['source'] }), activation,
      invocationCount: count, executionList: true, complete, failed,
      outputs: complete && !failed ? [Object.assign(new R.RunBatchOutputT(), { port: 'out', count, executionList: true })] : [],
    }),
  })
  listener.applyRunEvent(Object.assign(new R.RunEventT(), { runId, valueType: R.RunEventValue.RunBatchChanged, value: changed }))
}
batch(1n, 2, false)
listener.applyRunEvent(Object.assign(new R.RunEventT(), {
  runId: 'run', valueType: R.RunEventValue.RunCardChanged,
  value: Object.assign(new R.RunCardChangedT(), { invocation: Object.assign(new R.InvocationSummaryT(), {
    card: Object.assign(new D.CardPathT(), { segments: ['source'] }), invocation: 1n,
    batchActivation: 1n, state: D.CardState.succeeded, outputsSent: ['out'],
  }) }),
}))
assert.equal(session.nodeData('source').state, D.CardState.running, 'one member succeeding does not complete its batch')
assert.equal(new Scene(session, []).wires[0].style, 'normal', 'member outputs stay private until the whole batch completes')
batch(1n, 2, true, true)
assert.equal(session.nodeData('source').state, D.CardState.failed)
assert.equal(new Scene(session, []).wires[0].count, undefined, 'failed batch does not publish an empty list')

for (const count of [1, 0]) {
  const provisional = Object.assign(new R.RunEventT(), {
    runId: 'run', valueType: R.RunEventValue.RunCardChanged,
    value: Object.assign(new R.RunCardChangedT(), { invocation: Object.assign(new R.InvocationSummaryT(), {
      card: Object.assign(new D.CardPathT(), { segments: ['source'] }), invocation: 7n,
      batchActivation: 3n, phase: D.Phase.evaluate, state: D.CardState.running,
    }) }),
  })
  if (count === 0) {
    listener.applyRunEvent(provisional)
    assert.ok(session.timeline.some((entry) => entry.invocation === 7n))
  }
  batch(BigInt(3 - count), count, true)
  if (count === 0) {
    listener.applyRunEvent(provisional)
    assert.equal(session.nodeData('source').invocation, 0n)
    assert.equal(session.nodeData('source').stage, undefined)
    assert.ok(!session.timeline.some((entry) => entry.invocation === 7n), 'empty list preparation leaves no phantom invocation')
    assert.ok(session.timeline.some((entry) => entry.invocation === 1n), 'an earlier activation keeps its invocation')
  }
  const [list, collected] = new Scene(session, []).wires
  assert.equal(list.executionList, true)
  assert.equal(list.count, count)
  assert.equal(collected.executionList, false, 'Collect returns an ordinary value on a thin wire')
  const markup = renderToStaticMarkup(<WireLayer width={300} height={200} wires={[
    { d: list.d, style: 'normal', executionList: list.executionList, countLabel: `×${list.count}`, countAt: { x: 50, y: 50 } },
    { d: collected.d, style: 'normal', executionList: collected.executionList },
  ]} />)
  assert.match(markup, new RegExp(`×${count}<`))
  assert.match(markup, /stroke-width="4\.4"/)
  assert.match(markup, /stroke-width="1\.6"/)
}
batch(99n, 99, true, false, 'previous-run')
assert.equal(session.executionBatch('source')?.invocationCount, 0)
session.executionRev++
assert.equal(new Scene(session, []).wires[0].executionList, false, 'old runtime shape does not apply to an edited workflow')
const workspace = new Workspace()
workspace.logRunId = 'previous-run'
workspace.showLog('source', undefined, 1n, 'run')
assert.equal(workspace.logRunId, 'run', 'an invocation on the canvas opens logs for its own run')
assert.equal(workspace.logInvocation, 1n)
console.log('Execution lists: aggregate status, empty and single-item thick wires, and Collect passed')
