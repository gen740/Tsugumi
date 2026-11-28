// The log view's pipeline line draws the Card's stage circles and says in a
// second line what the running stage does (the same cases as the Mac's
// thePipelineLineDrawsEachStageAsTheCardDoes).
import assert from 'node:assert/strict'

const { Stage } = await import('../src/model/canvasKit')
const { formatAgo, stagePipeline } = await import('../src/model/stagePipeline')
const { D } = await import('../src/wire/rpc')
type CardRunState = import('../src/model/session').CardRunState
type StageRecord = import('../src/model/session').StageRecord

const order = [Stage.prepare, Stage.evaluate, Stage.build, Stage.execute, Stage.validate, Stage.save]
const record = (stage: number, outcome: number, start: number, end?: number): StageRecord => ({ stage, outcome, start, end })
const step = (line: ReturnType<typeof stagePipeline>, stage: number) => line.steps.find((s) => s.stage === stage)!

// Not run: every circle is an empty ring, with no time and no second line.
const none = stagePipeline(order, [], undefined, undefined, 0)
assert.deepEqual(none.steps.map((s) => s.stage), order)
assert.ok(none.steps.every((s) => s.status === 'pending' && s.seconds === undefined && !s.waiting) && none.detail === undefined)

// Running on a worker that builds the Card: the build circle runs, execution has not started yet.
const records = [record(Stage.prepare, D.StageOutcome.done, 0, 41_910), record(Stage.evaluate, D.StageOutcome.done, 41_910, 49_000),
  record(Stage.execute, D.StageOutcome.running, 49_000)]
const live: CardRunState = {
  state: D.CardState.running, invocation: 1n, phase: D.Phase.execute, executionService: 'ttl',
  executionActivity: D.ExecutionActivity.building, executionActivityDetail: 'rcw86-nustar-events',
  cpuMs: 341_230, lastOutputAt: 100_000 - 85_190, stages: records,
  cached: false, workerReused: false, sent: new Set(), sentInRun: new Set(),
}
const building = stagePipeline(order, records, 'ttl', live, 100_000)
assert.equal(step(building, Stage.prepare).status, 'done')
assert.equal(step(building, Stage.prepare).seconds, 41.91)
assert.deepEqual({ ...step(building, Stage.build) }, { stage: Stage.build, status: 'running', waiting: false, seconds: 51, onWorker: true })
assert.equal(step(building, Stage.execute).status, 'pending')
assert.equal(step(building, Stage.execute).seconds, undefined)
assert.equal(building.detail, 'ttl で Nix ビルド中（rcw86-nustar-events） · CPU 5:41 · 最後の出力 1 分 25 秒前')
assert.equal(formatAgo(12), '12 秒前')
assert.equal(formatAgo(7500), '2 時間 5 分前')

// Waiting for the worker's Nix store: the build circle waits (dashed).
const store = stagePipeline(order, records, 'ttl', { ...live, executionActivity: D.ExecutionActivity.waiting_store }, 100_000)
assert.ok(step(store, Stage.build).status === 'running' && step(store, Stage.build).waiting)
assert.match(store.detail!, /^ttl の Nix ストアを待っています/)
// Waiting for a free worker slot: nothing reached the worker; the execution waits.
const queued = stagePipeline(order, records, 'ttl',
  { ...live, executionActivity: D.ExecutionActivity.unspecified, executionState: D.RemoteExecutionState.queued }, 100_000)
assert.equal(step(queued, Stage.build).status, 'pending')
assert.ok(step(queued, Stage.execute).status === 'running' && step(queued, Stage.execute).waiting)
// The Card's program runs: the worker's build is behind it.
const running = stagePipeline(order, records, 'ttl',
  { ...live, executionActivity: D.ExecutionActivity.running, executionState: D.RemoteExecutionState.running }, 100_000)
assert.equal(step(running, Stage.build).status, 'done')
assert.ok(step(running, Stage.execute).status === 'running' && step(running, Stage.execute).seconds === 51)
assert.match(running.detail!, /^ttl で実行中 · CPU 5:41/)
// After the Card ran, the worker uploads its build outputs: the build stays done.
const pushing = stagePipeline(order, records, 'ttl',
  { ...live, executionActivity: D.ExecutionActivity.pushing, executionActivityDetail: 'remote-card-plan (2/5 · 1.2 GB / 4.0 GB)' }, 100_000)
assert.equal(step(pushing, Stage.build).status, 'done')
assert.equal(step(pushing, Stage.execute).status, 'running')
assert.match(pushing.detail!, /^ttl からビルド出力をキャッシュへ送信中（remote-card-plan \(2\/5 · 1\.2 GB \/ 4\.0 GB\)）/)

// Failed: the failed stage is red, the later ones never ran, no second line.
const failed = stagePipeline(order, [record(Stage.prepare, D.StageOutcome.done, 0, 10), record(Stage.evaluate, D.StageOutcome.failed, 10, 2_010)],
  undefined, undefined, 5_000)
assert.ok(step(failed, Stage.evaluate).status === 'failed' && step(failed, Stage.evaluate).seconds === 2)
assert.ok(step(failed, Stage.build).status === 'pending' && failed.detail === undefined)

// Succeeded with a cached build, on the project's own environment.
const cachedRecords = [record(Stage.prepare, D.StageOutcome.done, 0, 100), record(Stage.evaluate, D.StageOutcome.done, 100, 200),
  record(Stage.build, D.StageOutcome.cached, 200, 200), record(Stage.execute, D.StageOutcome.done, 200, 4_200),
  record(Stage.validate, D.StageOutcome.done, 4_200, 4_300), record(Stage.save, D.StageOutcome.done, 4_300, 4_310)]
const cached = stagePipeline(order, cachedRecords, 'local', undefined, 9_000)
assert.ok(step(cached, Stage.build).status === 'cached' && !step(cached, Stage.build).onWorker)
assert.ok(step(cached, Stage.execute).status === 'done' && step(cached, Stage.execute).seconds === 4 && cached.detail === undefined)
// A finished run on a worker: its build is shown done on the worker.
const remote = stagePipeline(order, cachedRecords.filter((r) => r.stage !== Stage.build), 'ttl', undefined, 9_000)
assert.ok(step(remote, Stage.build).status === 'done' && step(remote, Stage.build).onWorker)

// Stopped while the worker still prepared (the Card never started): the build
// carries the stop and the whole time, the execution did not run.
const stoppedRecords = [record(Stage.prepare, D.StageOutcome.done, 0, 100), record(Stage.evaluate, D.StageOutcome.done, 100, 200),
  record(Stage.execute, D.StageOutcome.cancelled, 200, 290_200)]
for (const before of [D.ExecutionActivity.waiting_store, D.ExecutionActivity.preparing, D.ExecutionActivity.building, D.ExecutionActivity.substituting]) {
  const stopped = stagePipeline(order, stoppedRecords, 'ttl', undefined, 300_000, before)
  assert.deepEqual({ ...step(stopped, Stage.build) }, { stage: Stage.build, status: 'cancelled', waiting: false, seconds: 290, onWorker: true })
  assert.deepEqual([step(stopped, Stage.execute).status, step(stopped, Stage.execute).seconds], ['notRun', undefined])
}
const failedBefore = stagePipeline(order, [...stoppedRecords.slice(0, 2), record(Stage.execute, D.StageOutcome.failed, 200, 1_200)],
  'ttl', undefined, 2_000, D.ExecutionActivity.building)
assert.ok(step(failedBefore, Stage.build).status === 'failed' && step(failedBefore, Stage.execute).status === 'notRun')
// Stopped once the Card ran (or while it pushed its outputs after): the build is done, the execution stopped.
for (const after of [D.ExecutionActivity.running, D.ExecutionActivity.pushing]) {
  const stopped = stagePipeline(order, stoppedRecords, 'ttl', undefined, 300_000, after)
  assert.ok(step(stopped, Stage.build).status === 'done' && step(stopped, Stage.build).seconds === undefined)
  assert.ok(step(stopped, Stage.execute).status === 'cancelled' && step(stopped, Stage.execute).seconds === 290)
}

// An open section whose stream has not loaded yet says nothing (no "empty"
// wording that flashes before the text arrives); after a moment, that it loads.
{
  const page = globalThis as Record<string, unknown>
  page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
  page.window ??= globalThis
  page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
  page.EventSource ??= class { addEventListener() {} close() {} }
  const { sectionPlaceholder } = await import('../src/ui/logs')
  for (const section of ['build', 'execute', 'save'] as const) {
    assert.equal(sectionPlaceholder(section, { known: true, invocation: 1n, loaded: false, slow: false }), '')
    assert.equal(sectionPlaceholder(section, { known: false, loaded: false, slow: false }), '')
    assert.equal(sectionPlaceholder(section, { known: true, invocation: 1n, loaded: false, slow: true }), '読み込んでいます…')
  }
  assert.equal(sectionPlaceholder('execute', { known: true, invocation: 1n, loaded: true, slow: false }), '出力はありません')
  assert.equal(sectionPlaceholder('save', { known: true, invocation: 1n, loaded: true, slow: true }), '記録された出来事はありません')
  assert.equal(sectionPlaceholder('execute', { known: true, loaded: false, slow: false }), 'この実行では、この Card はまだ動いていません')
}

console.log('Log pipeline line: stage circles, worker build and plain status line passed')
