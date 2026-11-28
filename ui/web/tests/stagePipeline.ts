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

console.log('Log pipeline line: stage circles, worker build and plain status line passed')
