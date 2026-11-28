// The log view's pipeline line: the invocation's stages as the Card's face
// draws them (the same circles and colours), side by side on a thin line, each
// with its name and short time, and under them what the running stage does
// now in plain words. The same model as StagePipeline in the Mac app.
import { L } from '../i18n'
import { D } from '../wire/rpc'
import { mergeStageStatus, Stage, STAGE_INFO, StageProgress, StageStatus, unreachedMark, unreachedStage } from './canvasKit'
import { CardRunState, executionActivityText, formatShortDuration, StageRecord } from './session'

export interface PipelineStep {
  stage: Stage
  status: StageStatus
  /** Running, but waiting (for a free worker slot or its Nix store). */
  waiting: boolean
  seconds?: number
  /** The core records no build of its own: the worker builds the Card as part
   *  of its execution, and this circle stands for that. */
  onWorker: boolean
}

export interface StagePipeline {
  steps: PipelineStep[]
  /** What the running stage does now, e.g. "ttl で Nix ビルド中（hello-1.0）· CPU 5:41 · 最後の出力 1 分 25 秒前";
   *  once it ended, the CPU time it used ("CPU 5:41"). */
  detail?: string
}

export function pipelineStatus(outcome: D.StageOutcome | undefined): StageStatus {
  switch (outcome) {
    case D.StageOutcome.running: return 'running'
    case D.StageOutcome.failed: return 'failed'
    case D.StageOutcome.cancelled: return 'cancelled'
    case D.StageOutcome.cached: return 'cached'
    case D.StageOutcome.reused: return 'reused'
    case undefined: case D.StageOutcome.unspecified: return 'pending'
    default: return 'done'
  }
}

/** How long ago, in words: "12 秒前", "1 分 25 秒前", "2 時間 5 分前". */
export function formatAgo(seconds: number): string {
  const s = Math.floor(Math.max(0, seconds))
  if (s < 60) return L('{0} 秒前', s)
  if (s < 3600) return L('{0} 分 {1} 秒前', Math.floor(s / 60), s % 60)
  return L('{0} 時間 {1} 分前', Math.floor(s / 3600), Math.floor(s / 60) % 60)
}

/** `stages`: the stages shown, in order; `records`: what the core recorded;
 *  `service`: "local", a worker, or undefined; `live`: the invocation's live
 *  state while the run goes on; `now`: the core's clock (ms since 1970);
 *  `cpuMs`: the CPU time it used, said once no stage runs any more;
 *  `buildCached`: whether the worker's build came from a store or cache
 *  (known once it ended); `cardState`: the Card's state, so a skipped or
 *  blocked Card's stages say so. */
export function stagePipeline(stages: Stage[], records: StageRecord[], service: string | undefined,
  live: CardRunState | undefined, now: number, lastActivity?: D.ExecutionActivity, cpuMs?: number, buildCached?: boolean,
  cardState?: D.CardState): StagePipeline {
  const record = (st: Stage) => [...records].reverse().find((r) => r.stage === st)
  const seconds = (r: StageRecord | undefined) => {
    if (r?.start === undefined) return undefined
    if (r.end !== undefined) return (r.end - r.start) / 1000
    return r.outcome === D.StageOutcome.running ? Math.max(0, now - r.start) / 1000 : undefined
  }
  const activity = executionActivityText(live)
  const steps: PipelineStep[] = stages.map((st) => {
    const r = record(st)
    return { stage: st, status: pipelineStatus(r?.outcome), waiting: false, seconds: seconds(r), onWorker: false }
  })
  const step = (st: Stage) => steps.find((s) => s.stage === st)
  const worker = workerStages(records, service, live, lastActivity ?? live?.executionLastActivity, buildCached)
  const build = step(Stage.build)
  if (build) {
    build.onWorker = worker.onWorker
    if (worker.build) Object.assign(build, {
      status: worker.build, waiting: worker.buildWaiting,
      // The worker's build is timed by the execution so far, or whole when
      // the invocation ended before the Card started.
      seconds: worker.build === 'running' || worker.endedInBuild ? seconds(record(Stage.execute)) : undefined,
    })
  }
  const run = step(Stage.execute)
  if (run) {
    if (worker.execute) Object.assign(run, { status: worker.execute, seconds: unreachedStage(worker.execute) ? undefined : run.seconds })
    run.waiting = worker.executeWaiting
  }
  const mark = unreachedMark(cardState ?? live?.state)
  if (mark) for (const s of steps) if (unreachedStage(s.status)) s.status = mark
  const current = steps.find((s) => s.status === 'running')
  if (!current && cpuMs !== undefined) return { steps, detail: L('CPU {0}', formatShortDuration(cpuMs / 1000)) }
  return { steps, detail: detail(current, worker.remote ? service : undefined, activity, live, now) }
}

/** A log section's state from the stages it covers, by the rule the circles
 *  follow: a section with a stage still running runs; once a later stage
 *  started, the section's stages that reported nothing are behind it. Its
 *  time is the sum of its stages' times, so the worker's build counts in the
 *  build section and not in the execution. The same rule as
 *  StagePipeline.section in the Mac app. `ended`: the invocation ended. */
export interface PipelineSection {
  status: StageStatus
  waiting: boolean
  seconds?: number
  cached: boolean
  reused: boolean
}

export function pipelineSection(line: StagePipeline, stages: Stage[], ended: boolean): PipelineSection {
  const mine = line.steps.filter((s) => stages.includes(s.stage))
  let last = -1
  line.steps.forEach((s, i) => { if (stages.includes(s.stage)) last = i })
  if (last < 0) return { status: 'pending', waiting: false, cached: false, reused: false }
  const later = line.steps.slice(last + 1).some((s) => !unreachedStage(s.status))
  const status = mergeStageStatus(mine.map((s) => s.status), ended || later)
  const times = mine.flatMap((s) => (s.seconds !== undefined ? [s.seconds] : []))
  return {
    status, waiting: status === 'running' && mine.some((s) => s.status === 'running' && s.waiting),
    seconds: times.length ? times.reduce((a, b) => a + b, 0) : undefined,
    cached: mine.some((s) => s.status === 'cached'), reused: mine.some((s) => s.status === 'reused'),
  }
}

/** One rule for the Card's face and the log's pipeline line, so that exactly
 *  one stage shows as running. On a worker the core records no build stage of
 *  its own: the worker builds the Card's derivation as part of the execution.
 *  While it prepares, builds or fetches it, or waits for its Nix store, the
 *  build is the running (or waiting) stage and the execution has not started;
 *  once the Card itself runs, the build is done. Waiting for a free worker
 *  slot is the execution's wait: nothing is built yet. A worker that runs the
 *  job but reports no activity built nothing before the Card (everything was
 *  in its store or substituted): once a later stage started, or the
 *  invocation ended after the Card ran, a build that reported no work shows
 *  as cached. An invocation that
 *  ended (failed or stopped) while the worker still waited for its store,
 *  prepared, built or fetched never ran the Card: the build carries that end
 *  and the time, and the execution did not run. The same rule as
 *  WorkerStages in the Mac app. */
export interface WorkerStages {
  /** The build circle stands for the worker's build. */
  onWorker: boolean
  /** What the build and execution circles show instead of the core's record. */
  build?: StageStatus
  buildWaiting: boolean
  execute?: StageStatus
  executeWaiting: boolean
  /** The execution runs on a worker, not in the project's own environment. */
  remote: boolean
  /** The invocation ended before the Card started: the build carries the
   *  end and the execution's time. */
  endedInBuild: boolean
}

/** What the worker does before the Card itself runs. */
const BEFORE_CARD = [D.ExecutionActivity.waiting_store, D.ExecutionActivity.preparing, D.ExecutionActivity.building, D.ExecutionActivity.substituting]

/** The worker's build once the Card is behind it: what the core recorded when
 *  the job ended, else cached when no build was ever reported (nothing at
 *  all, or only fetching), else done. */
export function buildBehindCard(lastActivity: D.ExecutionActivity | undefined, buildCached: boolean | undefined): StageStatus {
  if (buildCached !== undefined) return buildCached ? 'cached' : 'done'
  return lastActivity === undefined || lastActivity === D.ExecutionActivity.unspecified || lastActivity === D.ExecutionActivity.substituting
    ? 'cached' : 'done'
}

/** `lastActivity`: the last activity the remote job reported (kept after it
 *  ended); `buildCached`: whether the build came from a store or cache (known
 *  once it ended). */
export function workerStages(records: StageRecord[], service: string | undefined, live: CardRunState | undefined,
  lastActivity?: D.ExecutionActivity, buildCached?: boolean): WorkerStages {
  const remote = !!service && service !== 'local'
  const activity = executionActivityText(live)
  const execute = [...records].reverse().find((r) => r.stage === Stage.execute)
  const w: WorkerStages = { onWorker: false, buildWaiting: false, executeWaiting: false, remote, endedInBuild: false }
  if (!remote || !execute || records.some((r) => r.stage === Stage.build)) {
    // The build was the core's own (or kept): only the execution can wait.
    w.executeWaiting = execute?.outcome === D.StageOutcome.running && activity?.waiting === true
    return w
  }
  w.onWorker = true
  if (execute.outcome === D.StageOutcome.running) {
    // Publishing the build outputs follows the Card's run: the build is done.
    if (live?.executionActivity === D.ExecutionActivity.running || live?.executionActivity === D.ExecutionActivity.pushing) w.build = 'done'
    else if (activity && !activity.queued) Object.assign(w, { build: 'running', buildWaiting: activity.waiting, execute: 'pending' })
    // The worker runs the job and reports nothing before the Card: it had nothing to build.
    else if (live?.executionState === D.RemoteExecutionState.running) w.build = buildBehindCard(lastActivity ?? live.executionLastActivity, buildCached)
    else Object.assign(w, { build: 'pending', executeWaiting: activity?.queued === true })
  } else if (execute.outcome === D.StageOutcome.done) w.build = buildBehindCard(lastActivity, buildCached)
  else if (execute.outcome === D.StageOutcome.failed || execute.outcome === D.StageOutcome.cancelled) {
    if (lastActivity !== undefined && BEFORE_CARD.includes(lastActivity)) {
      Object.assign(w, { build: execute.outcome === D.StageOutcome.failed ? 'failed' : 'cancelled', execute: 'notRun', endedInBuild: true })
    } else if (lastActivity === D.ExecutionActivity.running || lastActivity === D.ExecutionActivity.pushing) w.build = buildBehindCard(lastActivity, buildCached)
  }
  return w
}

/** The build and execution circles of a Card's face as workerStages says:
 *  the worker's build runs before the Card does, never both at once. */
export function onWorker(progress: StageProgress, worker: WorkerStages): StageProgress {
  const run = progress.items.find((i) => i.stage === Stage.execute)
  return {
    ...progress,
    failedPart: worker.endedInBuild ? Stage.build : progress.failedPart,
    items: progress.items.map((item) => {
      if (item.stage === Stage.build && worker.build) {
        const seconds = worker.endedInBuild && run?.seconds !== undefined ? (item.seconds ?? 0) + run.seconds : item.seconds
        return { ...item, status: worker.build, waiting: worker.buildWaiting, seconds }
      }
      if (item.stage !== Stage.execute) return item
      const status = worker.execute ?? item.status
      return { ...item, status, waiting: worker.executeWaiting && status === 'running', seconds: worker.endedInBuild ? undefined : item.seconds }
    }),
  }
}

function detail(current: PipelineStep | undefined, service: string | undefined,
  activity: ReturnType<typeof executionActivityText>, live: CardRunState | undefined, now: number): string | undefined {
  if (!current || !live) return
  if (!current.onWorker && current.stage !== Stage.execute) return STAGE_INFO[current.stage].active
  const worker = service ?? L('ワーカー')
  const parts: string[] = []
  if (activity) {
    const what = activity.detail
    if (activity.queued) parts.push(L('{0} の空きを待っています', worker))
    else switch (activity.activity) {
      case D.ExecutionActivity.waiting_store: parts.push(L('{0} の Nix ストアを待っています（別の実行が使用中）', worker)); break
      case D.ExecutionActivity.preparing: parts.push(L('{0} でビルドの準備中', worker)); break
      case D.ExecutionActivity.building: parts.push(what ? L('{0} で Nix ビルド中（{1}）', worker, what) : L('{0} で Nix ビルド中', worker)); break
      case D.ExecutionActivity.substituting:
        parts.push(what ? L('{0} でビルド済みのものを取得中（{1}）', worker, what) : L('{0} でビルド済みのものを取得中', worker)); break
      case D.ExecutionActivity.pushing:
        parts.push(what ? L('{0} からビルド出力をキャッシュへ送信中（{1}）', worker, what) : L('{0} からビルド出力をキャッシュへ送信中', worker)); break
    }
  } else parts.push(service ? L('{0} で実行中', service) : L('実行中'))
  if (live.cpuMs !== undefined) parts.push(L('CPU {0}', formatShortDuration(live.cpuMs / 1000)))
  if (live.lastOutputAt !== undefined) parts.push(L('最後の出力 {0}', formatAgo(Math.max(0, now - live.lastOutputAt) / 1000)))
  else if (current.stage === Stage.execute && !activity) parts.push(L('出力はまだありません'))
  return parts.join(' · ')
}
