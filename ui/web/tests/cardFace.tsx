// An ordinary Card's face: its name (two lines at most), one status line and
// the stage circles on the right. Its size is the same in every state and for
// any name; the rest is on hover. The Mac app checks the same (UILogicTests:
// aCardsFaceKeepsItsSizeWhateverItsState, aCardsStatusLineSaysItsStateInOneLine,
// theHoverHasWhatTheFaceLeavesOut).
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { projectFixture } from './documentFixture'
import { addCodeCard, cardIds } from './cardIds'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { CardGeometry, Stage, STAGE_INFO } = await import('../src/model/canvasKit')
const { ProjectSession, cardState, formatShortDuration, relativeAge, stageProgressOf } = await import('../src/model/session')
const { stagePipeline } = await import('../src/model/stagePipeline')
const { CardNodeView, statusLine } = await import('../src/ui/cardNode')
const { cardHoverContent, hoverContent } = await import('../src/ui/canvas')
const { D } = await import('../src/wire/rpc')
const { failureText, notStartedText } = await import('../src/model/workerSelection')
const { parse } = await import('../shared/json')
type Data = import('../src/model/canvasKit').CardNodeData

const card = (title = 'x'): Data => ({
  title, kind: 'code', summary: 'what it does', inputs: [{ name: 'a' }], outputs: [{ name: 'b' }],
  invocation: 0n, runContinues: false, errors: [], warnings: [], collapsed: false,
})
const stages = (build: string, run: string, validate: string) => ({
  items: [{ stage: Stage.build, status: build }, { stage: Stage.execute, status: run }, { stage: Stage.validate, status: validate }],
  workerReused: false,
}) as Data['stage']

const running: Data = { ...card(), state: D.CardState.running, invocation: 1n, stage: stages('done', 'running', 'pending') }
const queued: Data = { ...card(), state: D.CardState.preparing, invocation: 1n, activity: 'waiting for a worker' }
const now = Date.now()
const succeeded: Data = { ...card(), state: D.CardState.succeeded, invocation: 3n, endedAt: now - 180_000, seconds: 4, stage: stages('cached', 'done', 'done') }
const failed: Data = {
  ...card(), state: D.CardState.failed, invocation: 1n, failure: '\nerror: no such file\n  at main.py:3', stage: stages('done', 'failed', 'notRun'),
}
const long = card('a long name of a Card '.repeat(6))

{
  // The same size whatever the state and the name.
  for (const flow of ['topToBottom', 'leftToRight'] as const) {
    const size = new CardGeometry(card(), flow).size
    for (const d of [running, queued, succeeded, failed, long]) assert.deepEqual(new CardGeometry(d, flow).size, size, `${d.title} ${flow}`)
  }
  const g = new CardGeometry(card(), 'topToBottom')
  assert.ok(g.face && !g.summary, 'the description is on hover')
}

{
  // One status line per state.
  assert.equal(statusLine(card()).text, '未実行')
  assert.equal(statusLine(running).text, STAGE_INFO[Stage.execute].active)
  assert.equal(statusLine(running).color, cardState(D.CardState.running).color)
  assert.equal(statusLine(queued).text, 'waiting for a worker')
  assert.equal(statusLine(succeeded, now).text, `${cardState(D.CardState.succeeded).label} ×3 · ${relativeAge(now - 180_000, now)} · 4.0 s`)
  assert.equal(statusLine(succeeded, now).color, 'secondary')
  assert.equal(statusLine(failed).text, 'run: no such file')
  assert.equal(statusLine(failed).color, cardState(D.CardState.failed).color)
  assert.deepEqual([formatShortDuration(4), formatShortDuration(192), formatShortDuration(3723)], ['4.0 s', '3:12', '1:02:03'])
}

{
  // The face: name, status line and three circles; no description.
  const html = renderToStaticMarkup(<CardNodeView data={failed} flow="topToBottom" />)
  assert.ok(html.includes('cv-face') && html.includes('no such file'))
  assert.equal(html.match(/class="cv-stage"/g)?.length, 3)
  // Read out to assistive technology, not drawn.
  assert.ok(!html.includes('>what it does<') && html.includes('aria-label="Card x'))
  // A Card in Card keeps its kind's symbol instead of circles.
  const nested = renderToStaticMarkup(<CardNodeView data={{ ...card('nested'), kind: 'subgraph', symbol: 'link' }} flow="topToBottom" />)
  assert.ok(nested.includes('cv-face') && !nested.includes('cv-stage'))
}

{
  // The hover has the description, the stages and the whole failure with a way to the log.
  const s = cardIds(new ProjectSession('/p', 'k', 'p'))
  s.project = parse(projectFixture({ name: 't', canvas: { cards: [], connections: [] } }))
  s.layout = parse('{}')
  s.connected = true
  const k = await addCodeCard(s, { x: 0, y: 0 })
  s.updateMeta(k, (m) => m.set('description', 'what it does'))
  await s.save()
  const idle = hoverContent(s, [], { kind: 'card', k })
  assert.deepEqual(idle.lines, ['what it does'])
  assert.equal(idle.openLog, undefined)
  assert.equal(idle.failure, undefined)
  const hover = cardHoverContent(failed, ['what it does'], '0123456789', () => {})
  assert.deepEqual(hover.lines, ['what it does'])
  assert.deepEqual(hover.stages?.map((i) => i.stage), [Stage.build, Stage.execute, Stage.validate])
  assert.equal(hover.failure, failed.failure)
  assert.ok(hover.openLog)
  assert.deepEqual(hover.problems, [])

  // A remote execution waiting for its worker: a dashed ring, the short form
  // on the face, the full one in the hover with where it runs and whether it works.
  const waits: Data = {
    ...card(), state: D.CardState.running, invocation: 1n,
    stage: { ...stages('done', 'running', 'pending'), items: stages('done', 'running', 'pending').items.map((i) => i.stage === Stage.execute ? { ...i, waiting: true } : i) },
    activity: 'waiting (worker slot)', activityDetail: 'waiting for a free slot on the worker', worker: 'ttl', liveness: 'CPU 00:00:01.00',
  }
  assert.equal(statusLine(waits).text, 'waiting (worker slot)')
  assert.deepEqual(new CardGeometry(waits, 'topToBottom').size, new CardGeometry(card(), 'topToBottom').size)
  assert.ok(renderToStaticMarkup(<CardNodeView data={waits} flow="topToBottom" />).includes('cv-dot-waiting'))
  const live = cardHoverContent(waits, [])
  assert.ok(live.stages?.some((i) => i.stage === Stage.execute && i.waiting))
  assert.ok(live.facts?.includes('waiting for a free slot on the worker') && live.facts.includes('CPU 00:00:01.00'))
  assert.ok(live.facts?.some((f) => f.endsWith(': ttl')))
}
{
  // No worker could be chosen: the Card never started. The face says so in
  // red on its one line, the hover each worker's reason; the size stays.
  const selection = Object.assign(new D.FailureT(), { kind: D.FailureKind.unsupported, diagnostic: 'No worker could be chosen' })
  selection.args = [['stage', 'worker_selection'], ['worker:ttl', 'timeout'], ['worker:gpu', 'failed']]
    .map(([key, value]) => Object.assign(new D.KeyValueT(), { key, value }))
  const reasons = notStartedText(selection)
  assert.equal(reasons, 'ttl の probe が時間切れ、gpu の probe が失敗')
  assert.equal(notStartedText(Object.assign(new D.FailureT(), { diagnostic: 'no such file' })), undefined)
  const notStarted: Data = {
    ...card(), state: D.CardState.failed, invocation: 1n, failure: failureText(selection), notStarted: reasons,
    stage: stages('notRun', 'notRun', 'notRun'),
  }
  assert.deepEqual(statusLine(notStarted), { text: 'worker を選べず未開始', color: cardState(D.CardState.failed).color, hover: reasons })
  for (const flow of ['topToBottom', 'leftToRight'] as const) {
    assert.deepEqual(new CardGeometry(notStarted, flow).size, new CardGeometry(card(), flow).size)
  }
  const html = renderToStaticMarkup(<CardNodeView data={notStarted} flow="topToBottom" />)
  assert.ok(html.includes('>worker を選べず未開始<') && html.includes(`title="${reasons}"`))
  assert.equal(statusLine(failed).hover, undefined)
}
{
  // A worker built from another revision is named as out of date, with both
  // contracts, and settings say how to bring it up to date.
  const selection = Object.assign(new D.FailureT(), { kind: D.FailureKind.unsupported, diagnostic: 'No worker could be chosen' })
  selection.args = [['stage', 'worker_selection'], ['worker:ttl', 'outdated'], ['contract:ttl', '21'], ['contract', '22'],
    ['worker:local', 'timeout']].map(([key, value]) => Object.assign(new D.KeyValueT(), { key, value }))
  assert.equal(notStartedText(selection),
    'ttl は古い版です（契約 21、この Tsugumi は 22）。ワーカーを同じ版で作り直してください、プロジェクトの実行環境 の probe が時間切れ')
  const { outdatedText } = await import('../src/ui/executionServices')
  const H = await import('../shared/gen/tsugumi/host')
  const capabilities = Object.assign(new H.ExecutionCapabilitiesT('server', 'x86_64-linux'), { workerContract: 21, revision: '0ld0rev' })
  const entry = Object.assign(new H.ExecutionServiceT('ttl', 'ttl', 'tls://ttl:8765', 'ed25519:x', capabilities), { outdated: true })
  assert.equal(outdatedText(entry, 22),
    'このワーカーは古い版です（契約 21、この Tsugumi は 22）。ワーカーを同じ版で作り直して再起動し、「接続を確認して更新」を押してください。')
  assert.equal(outdatedText(Object.assign(entry, { outdated: false }), 22), undefined)
}
{
  // On a worker no two stage circles run at once: while the worker builds
  // the Card its build circle runs and its run circle has not started; once
  // the Card runs, the build is done. The log's pipeline line agrees.
  const records = [
    { stage: Stage.prepare, outcome: D.StageOutcome.done, start: 0, end: 10 },
    { stage: Stage.evaluate, outcome: D.StageOutcome.done, start: 10, end: 20 },
    { stage: Stage.execute, outcome: D.StageOutcome.running, start: 20 },
  ]
  const live = {
    state: D.CardState.running, invocation: 1n, phase: D.Phase.execute, executionService: 'ttl', stages: records,
    cached: false, workerReused: false, sent: new Set<string>(), sentInRun: new Set<string>(),
  }
  const circles = (activity: number, queued = false) => {
    const state = { ...live, executionActivity: activity, executionState: queued ? D.RemoteExecutionState.queued : D.RemoteExecutionState.running }
    const progress = stageProgressOf(state, true)
    assert.ok(progress.items.filter((i) => i.status === 'running').length <= 1, `two circles running for activity ${activity}`)
    const line = stagePipeline([Stage.prepare, Stage.evaluate, Stage.build, Stage.execute, Stage.validate, Stage.save], records, 'ttl', state, 100)
    assert.ok(line.steps.filter((s) => s.status === 'running').length <= 1)
    return Object.fromEntries(progress.items.map((i) => [i.stage, i]))
  }
  for (const activity of [D.ExecutionActivity.preparing, D.ExecutionActivity.building, D.ExecutionActivity.substituting]) {
    const c = circles(activity)
    assert.ok(c[Stage.build].status === 'running' && !c[Stage.build].waiting && c[Stage.execute].status === 'pending')
  }
  const store = circles(D.ExecutionActivity.waiting_store)
  assert.ok(store[Stage.build].status === 'running' && store[Stage.build].waiting && store[Stage.execute].status === 'pending')
  const queued = circles(D.ExecutionActivity.unspecified, true)
  assert.ok(queued[Stage.build].status === 'pending' && queued[Stage.execute].status === 'running' && queued[Stage.execute].waiting)
  const running = circles(D.ExecutionActivity.running)
  assert.ok(running[Stage.build].status === 'done' && running[Stage.execute].status === 'running' && !running[Stage.execute].waiting)
  // The project's own environment builds first, as the core records it.
  const local = stageProgressOf({ ...live, executionService: 'local', stages: [records[0], { stage: Stage.evaluate, outcome: D.StageOutcome.running, start: 10 }] }, true)
  assert.deepEqual(local.items.filter((i) => i.status === 'running').map((i) => i.stage), [Stage.build])
  // Stopped while the worker prepared: the face agrees with the log's
  // pipeline line (build stopped with the time, run not run).
  const stopped = { ...live, state: D.CardState.cancelled, executionLastActivity: D.ExecutionActivity.preparing,
    stages: [records[0], records[1], { stage: Stage.execute, outcome: D.StageOutcome.cancelled, start: 20, end: 290_020 }] }
  const face = Object.fromEntries(stageProgressOf(stopped, true).items.map((i) => [i.stage, i]))
  assert.ok(face[Stage.build].status === 'cancelled' && face[Stage.build].seconds === 290.02)
  assert.ok(face[Stage.execute].status === 'notRun' && face[Stage.execute].seconds === undefined)
  assert.equal(stageProgressOf(stopped, true).failedPart, Stage.build)
  const line = stagePipeline([Stage.prepare, Stage.evaluate, Stage.build, Stage.execute], stopped.stages, 'ttl', stopped, 300_000)
  assert.deepEqual(line.steps.map((s) => s.status), ['done', 'done', 'cancelled', 'notRun'])
  // The worker runs the Card but reported no build (everything was in its
  // store or substituted): the build circle is not left empty behind the
  // running one; it takes the cached look, on the face and in the pipeline line.
  const nothingBuilt = circles(D.ExecutionActivity.unspecified)
  assert.ok(nothingBuilt[Stage.build].status === 'cached' && nothingBuilt[Stage.execute].status === 'running')
  const fetched = { ...live, executionState: D.RemoteExecutionState.running, executionActivity: D.ExecutionActivity.unspecified,
    executionLastActivity: D.ExecutionActivity.substituting }
  assert.equal(Object.fromEntries(stageProgressOf(fetched, true).items.map((i) => [i.stage, i]))[Stage.build].status, 'cached')
  const nothingLine = stagePipeline([Stage.prepare, Stage.evaluate, Stage.build, Stage.execute], records, 'ttl',
    { ...live, executionState: D.RemoteExecutionState.running, executionActivity: D.ExecutionActivity.unspecified }, 100)
  assert.deepEqual(nothingLine.steps.map((s) => s.status), ['done', 'done', 'cached', 'running'])
  // Ended: the core's record of whether the build came from a store decides.
  const succeeded = { ...live, state: D.CardState.succeeded, executionLastActivity: D.ExecutionActivity.running,
    stages: [records[0], records[1], { stage: Stage.execute, outcome: D.StageOutcome.done, start: 20, end: 3_020 }] }
  const ended = (cached: boolean) => Object.fromEntries(stageProgressOf({ ...succeeded, cached }, true).items.map((i) => [i.stage, i]))
  assert.equal(ended(true)[Stage.build].status, 'cached')
  assert.equal(ended(false)[Stage.build].status, 'done')
}
{
  // Every per-stage and per-section mark is the Card's stage circle: the log's
  // section headers, the pipeline line, the hover's stage table and the Card
  // state marks (sidebar, Card header) draw StageDot, never a spinner or a symbol.
  const { CardStateMark, cardStateStage, StageDot, StatusSymbol } = await import('../src/ui/statusSymbol')
  const dot = (status: string, waiting = false) => renderToStaticMarkup(<StageDot status={status as never} size={11} waiting={waiting} />)
  const cases: [string, string, boolean][] = [
    ['pending', 'pending', false], ['waiting', 'running', true], ['running', 'running', false], ['done', 'done', false],
    ['cached', 'cached', false], ['reused', 'reused', false], ['failed', 'failed', false], ['cancelled', 'cancelled', false],
  ]
  for (const [section, status, waiting] of cases) {
    const html = renderToStaticMarkup(<StatusSymbol status={section as never} />)
    assert.ok(html.includes(dot(status, waiting)), `section ${section} draws the stage circle`)
    assert.ok(!html.includes('status-spinner') && !html.includes('class="icon'), `no other glyph for ${section}`)
  }
  assert.ok(dot('running').includes('cv-dot-running') && dot('running', true).includes('cv-dot-waiting'))
  assert.ok(dot('cached').includes('cv-dot-cached') && dot('cancelled').includes('cv-dot-cancelled'))
  const look = (st: number) => cardStateStage(st)
  assert.deepEqual(look(D.CardState.succeeded), { status: 'done', waiting: false })
  assert.deepEqual(look(D.CardState.running), { status: 'running', waiting: false })
  assert.deepEqual(look(D.CardState.preparing), { status: 'running', waiting: false })
  assert.deepEqual(look(D.CardState.waiting), { status: 'running', waiting: true })
  assert.deepEqual(look(D.CardState.failed), { status: 'failed', waiting: false })
  assert.deepEqual(look(D.CardState.cancelled), { status: 'cancelled', waiting: false })
  assert.equal(look(D.CardState.skipped), undefined)
  assert.ok(renderToStaticMarkup(<CardStateMark state={D.CardState.succeeded} size={10} />).includes('cv-dot-done'))
}

{
  // The start Card's button runs, and stops while a run is on (the run
  // capsule has no button); the root one is the guide's "run". Its state is
  // one stage circle, the run gear while it is under way.
  const start = (state?: number): Data => ({ ...card('start'), kind: 'trigger', inputs: [], state })
  const draw = (d: Data, stop: boolean, enabled = true) => renderToStaticMarkup(
    <CardNodeView data={d} flow="leftToRight" start={{ stop, enabled, help: 'h' }} rootStart onRun={() => {}} onStop={() => {}} />)
  const idle = draw(start(D.CardState.succeeded), false)
  assert.match(idle, /class="cv-start-run"[^>]*data-guide="run"/)
  assert.match(idle, /aria-label="実行"/)
  assert.match(idle, /cv-dot-done/)
  const running = draw(start(D.CardState.running), true)
  assert.match(running, /class="cv-start-run cv-start-stop"/)
  assert.match(running, /aria-label="停止"/)
  assert.match(running, /class="run-gear"/)
  assert.match(draw(start(D.CardState.running), true, false), /<button[^>]*cv-start-stop[^>]*disabled=""/)
  assert.match(draw(start(D.CardState.failed), false), /cv-dot-failed/)
  assert.match(draw(start(D.CardState.cancelled), false), /cv-dot-cancelled/)
  assert.match(draw(start(), false), /class="cv-dot"/)
  // Not the root one: no guide target.
  assert.doesNotMatch(renderToStaticMarkup(<CardNodeView data={start()} flow="leftToRight" start={{ stop: false, enabled: true, help: 'h' }} />), /data-guide="run"/)
}
console.log('card face: passed')
