// Bottom panel: the whole run log, the run's Cards on a time axis, or a
// terminal in the project's folder. A Card's log is shown by stage.
import { useEffect, useRef, useState } from 'react'
import { LOG_READ_BYTES, readLogFile } from '../model/logWindow'
import { STAGE_INFO, Stage } from '../model/canvasKit'
import { useObserve } from '../model/observable'
import { CardKey, cardState, errorText, formatDuration, formatMillis, key, path, ProjectSession, runState, StageRecord, stageRecords, TimelineEntry } from '../model/session'
import { BOTTOM_ICONS, BOTTOM_TITLES, Workspace, WorkflowTab } from '../model/workspace'
import { D, R, RPCFailure, s } from '../wire/rpc'
import { Button, download, Empty, Segmented, Select, Sheet, Spinner } from './controls'
import { RunComparisonView } from './runComparison'
import { LogSource, LogText, useLogStream } from './logWindowView'
import { Icon } from './icons'
import { ProjectTerminalPanel } from './terminal'
import { L } from '../i18n'

const HEIGHT_KEY = 'tsugumi.bottomHeight'

export function LogTimelinePanel({ workspace, session, tab }: { workspace: Workspace; session: ProjectSession; tab: WorkflowTab }) {
  useObserve(workspace, session)
  const [height, setHeight] = useState(() => {
    try {
      return Math.max(140, Number(localStorage.getItem(HEIGHT_KEY) ?? 260) || 260)
    } catch {
      return 260
    }
  })
  const resize = (e: React.MouseEvent) => {
    e.preventDefault()
    const y0 = e.clientY, h0 = height
    const move = (m: MouseEvent) => {
      // The canvas keeps at least 240 points; the panel at least 140.
      const h = Math.max(140, Math.min(window.innerHeight - 240, h0 - (m.clientY - y0)))
      setHeight(h)
      try {
        localStorage.setItem(HEIGHT_KEY, String(h))
      } catch {}
    }
    const up = () => {
      window.removeEventListener('mousemove', move)
      window.removeEventListener('mouseup', up)
      document.body.classList.remove('resizing-v')
    }
    document.body.classList.add('resizing-v')
    window.addEventListener('mousemove', move)
    window.addEventListener('mouseup', up)
  }
  return (
    <div className="bottom-panel" style={{ height }}>
      <div className="vsplit-handle" onMouseDown={resize} />
      {workspace.bottomPanel === 'terminal'
        ? <ProjectTerminalPanel session={session} onClose={() => workspace.set('showBottom', false)} />
        : <RunPanel workspace={workspace} session={session} tab={tab} />}
    </div>
  )
}

function RunPanel({ workspace, session, tab }: { workspace: Workspace; session: ProjectSession; tab: WorkflowTab }) {
  const [runs, setRuns] = useState<R.RunSummaryT[]>([])
  const [comparing, setComparing] = useState(false)
  const runId = workspace.logRunId
  const selectRun = (id: string | undefined) => {
    if (workspace.logRunId === id) return
    workspace.set('logInvocation', undefined)
    workspace.set('logRunId', id)
  }
  const load = async () => {
    const rpc = session.rpc
    if (!rpc) return
    const list = (await rpc.history(null, false, 50).catch(() => undefined))?.runs ?? []
    setRuns(list)
    const current = workspace.logRunId
    if (!current || !list.some((r) => s(r.status!.runId) === current)) {
      selectRun(session.currentRunId ?? (list[0] ? s(list[0].status!.runId) : undefined))
    }
  }
  useEffect(() => {
    void load()
  }, [session.lastFinished, session.connected])
  useEffect(() => {
    const id = session.currentRunId
    if (id) {
      selectRun(id)
      void load()
    }
  }, [session.currentRunId])
  const p = workspace.bottomPanel
  const cards = [...session.resolution.keys()].sort()
  return (
    <>
      <div className="panel-bar">
        <span className="label headline callout"><Icon name={BOTTOM_ICONS[p]} />{BOTTOM_TITLES[p]}</span>
        <Select label={L('実行')} value={runId ?? ''} onChange={(v) => selectRun(v || undefined)} style={{ maxWidth: 320 }}
          options={runs.length ? runs.map((r) => ({ value: s(r.status!.runId), label: `${formatMillis(r.status!.acceptedAt)} · ${runState(r.status!.state).label}` })) : [{ value: '', label: L('実行履歴なし') }]} />
        {p === 'log' && (
          <Select label="Card" value={workspace.logCard ?? ''} onChange={(v) => { workspace.set('logInvocation', undefined); workspace.set('logCard', v || undefined) }} style={{ maxWidth: 240 }}
            options={[{ value: '', label: L('全体') }, ...cards.map((k) => ({ value: k, label: k }))]} />
        )}
        <span className="spacer" />
        <Button small disabled={runs.length < 2} onClick={() => setComparing(true)}>{L('実行を比較')}</Button>
        <Button kind="borderless" small iconOnly icon="xmark" help={L('閉じる（下部のアイコンでも開閉できます）')} onClick={() => workspace.set('showBottom', false)} />
      </div>
      {comparing && <Sheet width={1200} onClose={() => setComparing(false)}><RunComparisonView session={session} runs={runs} initial={runId} onClose={() => setComparing(false)} /></Sheet>}
      {p === 'log' ? (
        runId && workspace.logCard ? (
          <StageLogView session={session} runId={runId} card={path(workspace.logCard)} invocation={workspace.logInvocation} focus={workspace.logStage} />
        ) : runId ? (
          // The whole run: its own journal (Card logs are shown by stage).
          <RunLogView session={session} runId={runId} raw stream={workspace.logStream} onStream={(v) => workspace.set('logStream', v)} />
        ) : (
          <Empty title={L('実行履歴はありません')} icon="text.alignleft" description={L('実行するとここにログが表示されます。')} />
        )
      ) : (
        <RunTimelineView session={session} runId={runId} run={runs.find((r) => s(r.status!.runId) === runId)?.status ?? undefined}
          onSelect={(k) => {
            tab.enter(path(k).slice(0, -1))
            tab.setSelection([k])
            workspace.showLog(k)
          }} />
      )}
    </>
  )
}

function isLiveStream(session: ProjectSession, runId: string, stream: D.LogStream) {
  return session.runActive && runId === session.currentRunId &&
    (stream === D.LogStream.standard_output || stream === D.LogStream.standard_error)
}

function truncationNotice(limit: bigint) {
  return L('ログはここで切り詰めました（上限 {0} バイト）', limit)
}

/** Writes a whole stream to a file, in pieces. */
async function exportLog(session: ProjectSession, source: LogSource, size: number) {
  const rpc = session.rpc
  if (!rpc) throw new Error(L('実行環境に接続していません'))
  const file = await readLogFile(size, (offset) =>
    rpc.logRange(source.runId, source.card, source.invocation, source.stream, offset, false, LOG_READ_BYTES, null, false, null, source.live))
  download(`${source.runId}-${D.LogStream[source.stream]}.log`, file)
}

/** Log text of a run (or one Card / invocation of it), read incrementally
 *  with LogRange from where it stopped when LogAvailable arrives. A Card's
 *  log is shown by stage (StageLogView) unless `raw` asks for the streams. */
export function RunLogView({ session, runId, card, invocation, stage, raw, stream, onStream }: {
  session: ProjectSession; runId: string; card?: string[]; invocation?: bigint; stage?: Stage; raw?: boolean
  stream?: D.LogStream; onStream?: (s: D.LogStream) => void
}) {
  if (card && !raw) return <StageLogView session={session} runId={runId} card={card} invocation={invocation} focus={stage} />
  return <StreamLogView session={session} runId={runId} card={card} invocation={invocation} stream={stream} onStream={onStream} />
}

function StreamLogView({ session, runId, card, invocation, stream: shownStream, onStream }: {
  session: ProjectSession; runId: string; card?: string[]; invocation?: bigint; stream?: D.LogStream; onStream?: (s: D.LogStream) => void
}) {
  useObserve(session)
  const [ownStream, setOwnStream] = useState<D.LogStream>(card ? D.LogStream.standard_output : D.LogStream.journal)
  const stream = shownStream ?? ownStream
  const setStream = (v: D.LogStream) => (onStream ? onStream(v) : setOwnStream(v))
  const [resolved, setResolved] = useState<bigint>()
  const [exportError, setExportError] = useState<string>()
  // A Card's latest invocation, when none was named.
  useEffect(() => {
    setResolved(undefined)
    if (!card || invocation !== undefined) return
    const live = runId === session.currentRunId ? session.cardStates.get(key(card))?.invocation : undefined
    if (live !== undefined) return setResolved(live)
    let alive = true
    void session.rpc?.record(runId, card).then((r) => alive && setResolved(r.selectedInvocation ?? undefined)).catch(() => undefined)
    return () => { alive = false }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [runId, card?.join('/'), invocation])
  const source: LogSource = { runId, card: card ?? null, invocation: card ? (invocation ?? resolved ?? null) : null, stream, live: isLiveStream(session, runId, stream) }
  const log = useLogStream(session, source, 'terminal')

  /** Why nothing is shown, so an empty pane is never ambiguous. */
  const emptyReason = (() => {
    if (log.error) return L('ログを読み込めませんでした: ') + log.error
    if (!log.loaded) return L('読み込んでいます…')
    if (card && runId === session.currentRunId && !session.cardStates.has(key(card))) return L('この実行では、この Card はまだ動いていません。')
    switch (stream) {
      case D.LogStream.standard_error: return L('stderr への出力はありません（Nix 評価・build のログもここに出ます）。')
      case D.LogStream.journal: return L('記録された段階の出来事はありません。')
      default: return L('stdout への出力はありません。')
    }
  })()

  return (
    <div className="log-view">
      <div className="panel-bar">
        <Segmented<D.LogStream> small value={stream} onChange={setStream} width={240}
          options={[{ value: D.LogStream.standard_output, label: 'stdout' }, { value: D.LogStream.standard_error, label: 'stderr' }, { value: D.LogStream.journal, label: 'journal' }]} />
        <span className="caption secondary">{card ? `Card ${session.displayPath(card)}${invocation !== undefined ? ` #${invocation}` : ''}` : L('実行全体')}</span>
        <span className="spacer" />
        <Button small disabled={!log.size} onClick={() => void exportLog(session, source, log.rawSize).catch((e) => setExportError(errorText(e)))}>{L('書き出す…')}</Button>
      </div>
      <LogText log={log} placeholder={emptyReason} />
      {log.truncatedAt !== undefined && <div className="caption headline" style={{ padding: '0 6px' }}>{truncationNotice(log.truncatedAt)}</div>}
      {(log.error ?? exportError) && <div className="caption" style={{ color: 'var(--red)', padding: '0 6px' }}>{log.error ?? exportError}</div>}
    </div>
  )
}

// ---- Log by stage ---------------------------------------------------------------------

interface StageGroup {
  id: 'build' | 'execute' | 'save'
  title: string
  stages: Stage[]
}

type GroupStatus = { icon: string; color: string; label: string }

/** The log of one invocation of a Card by stage, like a CI job: SDK
 *  preparation, Nix evaluation and build / execution / validation and save,
 *  each with its state and duration; the chosen one's output fills the rest.
 *  The raw streams stay available under "By stream". Only the shown stream
 *  is read, from its end and then as it grows. */
export function StageLogView({ session, runId, card, invocation, focus }: {
  session: ProjectSession; runId: string; card: string[]; invocation?: bigint; focus?: Stage
}) {
  useObserve(session)
  const k = key(card)
  const [info, setInfo] = useState<R.RecordResultT>()
  const [infoError, setInfoError] = useState<string>()
  const [selected, setSelected] = useState<string>()
  const [rawStream, setRawStream] = useState<D.LogStream>(D.LogStream.standard_output)
  const [executeStream, setExecuteStream] = useState<D.LogStream>(D.LogStream.standard_output)
  const [exporting, setExporting] = useState(false)
  const [exportError, setExportError] = useState<string>()
  const [attempt, setAttempt] = useState(0)

  const code = session.kind(k) === 'code'
  const groups: StageGroup[] = [
    ...(code ? [{ id: 'build' as const, title: L('SDK 準備・Nix 評価・build'), stages: [Stage.prepare, Stage.evaluate, Stage.build] }] : []),
    { id: 'execute', title: L('実行'), stages: [Stage.execute] },
    { id: 'save', title: L('出力検証・保存'), stages: [Stage.validate, Stage.save] },
  ]
  const liveState = runId === session.currentRunId ? session.cardStates.get(k) : undefined
  const live = liveState && (invocation === undefined || liveState.invocation === invocation) ? liveState : undefined
  const shownInvocation = invocation ?? live?.invocation ?? info?.selectedInvocation ?? undefined
  const stages: StageRecord[] = live ? live.stages : info?.record?.summary ? stageRecords(info.record.summary.stages) : []
  const state = live?.state ?? info?.record?.summary?.state
  const failure = live?.failure ?? (info?.record?.summary?.failure?.diagnostic ? s(info.record.summary.failure.diagnostic) : undefined)

  const records = (g: StageGroup) => stages.filter((r) => g.stages.includes(r.stage))
  const status = (g: StageGroup): GroupStatus => {
    const rs = records(g)
    if (rs.some((r) => r.outcome === D.StageOutcome.failed)) return { icon: 'xmark.circle.fill', color: 'red', label: L('失敗') }
    if (rs.some((r) => r.outcome === D.StageOutcome.cancelled)) return { icon: 'stop.circle.fill', color: 'brown', label: L('停止') }
    if (rs.some((r) => r.outcome === D.StageOutcome.running)) return { icon: 'circle.dotted.circle', color: 'orange', label: L('進行中') }
    if (!rs.length) return { icon: 'circle.dashed', color: 'secondary', label: L('実行されていません') }
    if (rs.every((r) => r.outcome === D.StageOutcome.cached)) return { icon: 'bolt.circle.fill', color: 'blue', label: L('キャッシュを利用') }
    if (rs.every((r) => r.outcome === D.StageOutcome.reused)) return { icon: 'arrow.triangle.2.circlepath.circle.fill', color: 'teal', label: L('再利用') }
    return { icon: 'checkmark.circle.fill', color: 'green', label: L('完了') }
  }
  const seconds = (g: StageGroup): number | undefined => {
    const rs = records(g).flatMap((r) => (r.start !== undefined && r.end !== undefined ? [(r.end - r.start) / 1000] : []))
    return rs.length ? rs.reduce((a, b) => a + b, 0) : undefined
  }
  // Running, else failed, else the one asked for, else the execution.
  const defaultGroup = groups.find((g) => status(g).label === L('進行中'))?.id ?? groups.find((g) => status(g).label === L('失敗'))?.id
    ?? (focus !== undefined ? groups.find((g) => g.stages.includes(focus))?.id : undefined) ?? 'execute'
  const current = selected ?? defaultGroup
  const stream = current === 'build' ? D.LogStream.build : current === 'execute' ? executeStream : current === 'save' ? D.LogStream.journal : rawStream
  const source: LogSource | undefined = shownInvocation === undefined ? undefined : { runId, card, invocation: shownInvocation, stream, live: isLiveStream(session, runId, stream) }
  const log = useLogStream(session, source, current === 'save' ? 'journal' : 'terminal')

  // State changes (a stage starts or ends) read the record again; new output
  // is read as LogAvailable announces it.
  const execution = `${session.currentRunId}|${session.runStatus?.state}|${liveState?.state}|${liveState?.phase}|${liveState?.invocation}|${session.remoteHistoryRevision}`
  useEffect(() => {
    const rpc = session.rpc
    if (!rpc) return setInfoError(L('実行環境に接続していません'))
    let alive = true
    void rpc.record(runId, card, invocation ?? null, invocation === undefined).then((r) => {
      if (!alive) return
      setInfo(r)
      setInfoError(undefined)
    }).catch((e) => {
      if (!alive) return
      if (e instanceof RPCFailure && e.kind === D.FailureKind.not_found) { setInfo(undefined); setInfoError(undefined) } else setInfoError(errorText(e))
    })
    return () => { alive = false }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [runId, k, invocation, execution, attempt])
  useEffect(() => {
    setInfo(undefined)
    setInfoError(undefined)
    setSelected(undefined)
    setExportError(undefined)
  }, [runId, k, invocation])
  useEffect(() => {
    const g = focus !== undefined ? groups.find((g) => g.stages.includes(focus)) : undefined
    if (g) setSelected(g.id)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [focus])

  const placeholder = (() => {
    if (shownInvocation === undefined || (!log.loaded && !log.error)) {
      return !info && !live ? L('読み込んでいます…') : L('この実行では、この Card はまだ動いていません')
    }
    switch (current) {
      case 'build': return L('準備・評価・build の出力はありません（キャッシュや再利用のときは空です）')
      case 'save': return L('記録された出来事はありません')
      case 'execute': return executeStream === D.LogStream.standard_error ? L('stderr への出力はありません') : L('stdout への出力はありません')
      default: return L('この stream への出力はありません')
    }
  })()

  const chip = (id: string, help: string, content: JSX.Element) => (
    <button key={id} type="button" className={`stage-chip${current === id ? ' selected' : ''}`} title={help} aria-pressed={current === id} onClick={() => setSelected(id)}>{content}</button>
  )
  const shownGroup = groups.find((g) => g.id === current)
  const limit = log.truncatedAt

  let body: JSX.Element
  if (infoError && !info && !live) {
    body = (
      <Empty title={L('ログを読み込めませんでした')} icon="exclamationmark.triangle" description={infoError}>
        <Button onClick={() => setAttempt(attempt + 1)}>{L('再試行')}</Button>
      </Empty>
    )
  } else if (info?.exists === false && !live) {
    body = <Empty title={L('この実行では、この Card は呼び出されていません')} icon="circle.dotted" />
  } else {
    body = (
      <>
        <div className="row gap12 wrap stage-streams">
          {current === 'execute' && <Segmented<D.LogStream> small value={executeStream} onChange={setExecuteStream}
            options={[{ value: D.LogStream.standard_output, label: 'stdout' }, { value: D.LogStream.standard_error, label: 'stderr' }]} />}
          {current === 'raw' && <Segmented<D.LogStream> small value={rawStream} onChange={setRawStream} width={300}
            options={[{ value: D.LogStream.standard_output, label: 'stdout' }, { value: D.LogStream.standard_error, label: 'stderr' }, { value: D.LogStream.build, label: 'build' }, { value: D.LogStream.journal, label: 'journal' }]} />}
          {shownGroup?.stages.map((sg) => {
            const r = [...stages].reverse().find((x) => x.stage === sg)
            return (
              <span key={sg} className={`label caption${r ? '' : ' tertiary'}`}>
                <Icon name={STAGE_INFO[sg].icon} size={11} />
                {STAGE_INFO[sg].label}
                {r && r.start !== undefined && r.end !== undefined && <span className="secondary">{formatDuration((r.end - r.start) / 1000)}</span>}
                {r?.outcome === D.StageOutcome.cached && <span style={{ color: 'var(--blue)' }}>cache</span>}
                {r?.outcome === D.StageOutcome.reused && <span style={{ color: 'var(--teal)' }}>{L('再利用')}</span>}
              </span>
            )
          })}
        </div>
        {(log.error ?? exportError) && <div className="caption stage-notice" style={{ color: 'var(--red)' }}>{log.error ?? exportError}</div>}
        <LogText log={log} placeholder={placeholder} />
        {limit != null && <div className="caption headline stage-notice">{truncationNotice(limit)}</div>}
        {failure && shownGroup && status(shownGroup).label === L('失敗') && <div className="caption selectable stage-notice" style={{ color: 'var(--red)' }}>{failure}</div>}
      </>
    )
  }
  return (
    <div className="stage-log">
      <div className="panel-bar">
        <span className="headline callout ellipsis">
          {session.displayName(k)} · {L('{0} 回目', String(shownInvocation ?? 0n))}{state !== undefined ? ` · ${cardState(state).label}` : ''}
        </span>
        <span className="spacer" />
        {source && !!log.rawSize && <Button small disabled={exporting} onClick={() => {
          setExporting(true)
          setExportError(undefined)
          void exportLog(session, source, log.rawSize).catch((e) => setExportError(errorText(e))).finally(() => setExporting(false))
        }}>{L('ログを書き出す…')}</Button>}
      </div>
      <div className="row gap6 stage-chips">
        {groups.map((g) => {
          const st = status(g), sec = seconds(g)
          return chip(g.id, `${g.title} · ${st.label}`, <>
            <Icon name={st.icon} style={{ color: `var(--${st.color})` }} />
            <span>{g.title}</span>
            {sec !== undefined && <span className="secondary mono-digits">{formatDuration(sec)}</span>}
          </>)
        })}
        {chip('raw', L('stream 別（元の出力）'), <><Icon name="list.bullet.rectangle" /><span>{L('stream 別')}</span></>)}
      </div>
      {body}
    </div>
  )
}

// ---- Timeline ----------------------------------------------------------------------------

const LABEL_WIDTH = 170
const ROW_HEIGHT = 18
const STAGE_COLORS: Partial<Record<Stage, string>> = {
  [Stage.prepare]: 'var(--indigo)', [Stage.evaluate]: 'var(--cyan)', [Stage.build]: 'var(--blue)',
  [Stage.execute]: 'var(--green)', [Stage.validate]: 'var(--mint)', [Stage.save]: 'var(--teal)',
}

/** Invocations of one run on a shared time axis, one row per Card, each bar
 *  split into its stages. Drag over a time range to zoom into it, scroll
 *  sideways to move along time, "All" to see the whole run again. Hover a bar
 *  for its Card, invocation, stage and times; click it to show the Card. */
function RunTimelineView({ session, runId, run, onSelect }: { session: ProjectSession; runId?: string; run?: R.RunStatusT; onSelect: (k: CardKey) => void }) {
  const [entries, setEntries] = useState<TimelineEntry[]>([])
  const [loading, setLoading] = useState(false)
  /** Shown time range in ms from the start of the run (undefined: the whole run). */
  const [range, setRange] = useState<[number, number]>()
  const [drag, setDrag] = useState<[number, number]>()
  const [hover, setHover] = useState<{ x: number; y: number }>()
  const [width, setWidth] = useState(400)
  const [, tick] = useState(0)
  const chartRef = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!session.runActive) return
    const t = setInterval(() => tick((x) => x + 1), 500)
    return () => clearInterval(t)
  }, [session.runActive])
  useEffect(() => {
    const el = chartRef.current
    if (!el) return
    const ro = new ResizeObserver(() => setWidth(el.clientWidth || 400))
    ro.observe(el)
    return () => ro.disconnect()
  }, [])
  // A past run's invocations, read per Card with RunInvocations.
  useEffect(() => {
    setRange(undefined)
    setEntries([])
    const rpc = session.rpc
    if (!runId || !rpc || (runId === session.currentRunId && session.timeline.length)) return
    let alive = true
    setLoading(true)
    void (async () => {
      const out: TimelineEntry[] = []
      for (const k of [...session.resolution.keys()].sort().slice(0, 300)) {
        const page = await rpc.invocations(runId, path(k)).catch(() => undefined)
        for (const r of page?.records ?? []) {
          out.push({ card: k, invocation: r.invocation ?? 0n, state: r.state, startedAt: r.startedAt !== null ? Number(r.startedAt) : undefined,
            endedAt: r.endedAt !== null ? Number(r.endedAt) : undefined, stages: stageRecords(r.stages) })
        }
      }
      if (!alive) return
      setEntries(out)
      setLoading(false)
    })()
    return () => {
      alive = false
    }
  }, [runId])

  const current = runId !== undefined && runId === session.currentRunId
  const list = current && session.timeline.length ? session.timeline : entries
  if (!list.length) {
    return loading ? <div className="center-fill"><Spinner /></div>
      : <Empty title={L('実行状況はありません')} icon="chart.bar.xaxis" description={L('実行を選ぶか、実行してください。')} />
  }
  const rows = [...new Set(list.map((e) => e.card))].sort()
  const rowIndex = new Map(rows.map((k, i) => [k, i]))
  // The axis starts when the run was accepted and ends when it ended (or
  // now, on the core's clock), like the elapsed time of the run control.
  const status = current ? session.runStatus : run
  const live = current && session.runActive
  const now = session.coreNow()
  const starts = list.map((e) => e.startedAt).filter((x): x is number => x !== undefined)
  const start = status?.acceptedAt !== null && status?.acceptedAt !== undefined ? Number(status.acceptedAt) : starts.length ? Math.min(...starts) : 0
  const lastEnd = Math.max(start, ...list.map((e) => e.endedAt ?? (live ? now : e.startedAt ?? start)))
  const statusEnd = status?.endedAt !== null && status?.endedAt !== undefined ? Number(status.endedAt) : live ? now : lastEnd
  const end = Math.max(start + 1, statusEnd, lastEnd)
  const total = end - start
  const shown = range ?? [0, total]
  const span = Math.max(1, shown[1] - shown[0])
  const x = (t: number) => ((t - start - shown[0]) / span) * width

  const hit = (p: { x: number; y: number }): TimelineEntry | undefined => {
    const row = Math.floor(p.y / ROW_HEIGHT)
    return [...list].reverse().find((e) => {
      if (rowIndex.get(e.card) !== row || e.startedAt === undefined) return false
      const x0 = x(e.startedAt), x1 = Math.max(x0 + 2, x(e.endedAt ?? e.startedAt))
      return p.x >= x0 - 2 && p.x <= x1 + 2
    })
  }
  const hovered = hover && !drag ? hit(hover) : undefined
  const describe = (e: TimelineEntry) => {
    const lines = [L('{0} · 実行 #{1} · {2}', session.displayName(e.card), e.invocation, cardState(e.state).label)]
    if (e.startedAt !== undefined && e.endedAt !== undefined) lines.push(L('全体 ') + formatDuration((e.endedAt - e.startedAt) / 1000))
    for (const st of e.stages) if (st.start !== undefined && st.end !== undefined) lines.push(`${STAGE_INFO[st.stage].label}: ` + formatDuration((st.end - st.start) / 1000))
    return lines.join('\n')
  }
  const local = (e: React.MouseEvent) => {
    const r = chartRef.current!.getBoundingClientRect()
    return { x: e.clientX - r.left, y: e.clientY - r.top }
  }
  const down = (e: React.MouseEvent) => {
    if (e.button !== 0) return
    const p0 = local(e)
    let moved = false
    const move = (m: MouseEvent) => {
      const r = chartRef.current?.getBoundingClientRect()
      if (!r) return
      const px = m.clientX - r.left
      if (Math.abs(px - p0.x) >= 4) moved = true
      if (moved) setDrag([p0.x, px])
    }
    const up = (m: MouseEvent) => {
      window.removeEventListener('mousemove', move)
      window.removeEventListener('mouseup', up)
      setDrag(undefined)
      const r = chartRef.current?.getBoundingClientRect()
      if (!r) return
      const px = m.clientX - r.left
      if (!moved) {
        const h = hit(p0)
        if (h) onSelect(h.card)
        return
      }
      const a = Math.min(p0.x, px), b = Math.max(p0.x, px)
      if (b - a > 6) {
        const lo = shown[0] + (a / width) * span, hi = shown[0] + (b / width) * span
        setRange([Math.max(0, lo), Math.max(lo + 1, hi)])
      }
    }
    window.addEventListener('mousemove', move)
    window.addEventListener('mouseup', up)
  }
  // Sideways scroll moves along time (only when zoomed in).
  const wheel = (e: React.WheelEvent) => {
    if (!range) return
    const dx = Math.abs(e.deltaX) > Math.abs(e.deltaY) ? e.deltaX : e.shiftKey ? e.deltaY : 0
    if (!dx) return
    const shift = (dx / Math.max(width, 1)) * span
    const lo = Math.min(Math.max(0, range[0] + shift), Math.max(0, total - span))
    setRange([lo, lo + span])
  }

  return (
    <div className="timeline">
      <div className="timeline-head">
        <span className="caption2 secondary mono-digits" style={{ width: LABEL_WIDTH, textAlign: 'right' }}>{formatDuration(shown[0] / 1000)}</span>
        <span className="spacer" />
        <span className="caption2 tertiary">{L('ドラッグで範囲を拡大・横スクロールで移動')}</span>
        <Button small disabled={!range} help={L('Run 全体（受付から終了まで {0}）を表示', formatDuration(total / 1000))} onClick={() => setRange(undefined)}>{L('全体')}</Button>
        <span className="caption2 secondary mono-digits">{formatDuration(shown[1] / 1000) + (live && !range ? L('（実行中）') : '')}</span>
      </div>
      <div className="timeline-scroll">
        <div className="row">
          <div className="timeline-labels" style={{ width: LABEL_WIDTH }}>
            {rows.map((k) => {
              const n = list.filter((e) => e.card === k).length
              return (
                <div key={k} className="timeline-label caption" style={{ height: ROW_HEIGHT }} title={k} onClick={() => onSelect(k)}>
                  <span className="ellipsis flex1">{session.displayName(k)}</span>
                  {n > 1 && <span className="secondary mono-digits">×{n}</span>}
                </div>
              )
            })}
          </div>
          <div ref={chartRef} className="timeline-chart" style={{ height: rows.length * ROW_HEIGHT }}
            onMouseMove={(e) => setHover(local(e))} onMouseLeave={() => setHover(undefined)} onMouseDown={down} onWheel={wheel}>
            <svg width={width} height={rows.length * ROW_HEIGHT}>
              {rows.map((_, i) => i % 2 === 1 && <rect key={i} x={0} y={i * ROW_HEIGHT} width={width} height={ROW_HEIGHT} className="timeline-stripe" />)}
              {list.map((e) => {
                const row = rowIndex.get(e.card)
                if (row === undefined || e.startedAt === undefined) return null
                const f = e.endedAt ?? (live ? now : e.startedAt)
                const x0 = x(e.startedAt), x1 = Math.max(x0 + 2, x(f))
                if (x1 < 0 || x0 > width) return null
                const y = row * ROW_HEIGHT + 3, h = ROW_HEIGHT - 6
                return (
                  <g key={`${e.card}#${e.invocation}`}>
                    <rect x={x0} y={y} width={x1 - x0} height={h} rx={2} style={{ fill: `var(--${cardState(e.state).color})`, opacity: 0.35 }} />
                    {e.stages.map((st, i) => {
                      if (st.start === undefined) return null
                      const b = st.end ?? (live ? now : st.start)
                      const sx0 = x(st.start), sx1 = Math.max(sx0 + 1, x(b))
                      return <rect key={i} x={sx0} y={y} width={sx1 - sx0} height={h} style={{ fill: STAGE_COLORS[st.stage] ?? 'var(--gray)', opacity: 0.85 }} />
                    })}
                  </g>
                )
              })}
              {drag && <rect x={Math.min(drag[0], drag[1])} y={0} width={Math.abs(drag[1] - drag[0])} height={rows.length * ROW_HEIGHT} className="timeline-drag" />}
            </svg>
            {hovered && hover && (
              <div className="timeline-tip caption" style={{ left: Math.min(hover.x + 12, Math.max(0, width - 260)), top: hover.y + 12 }}>{describe(hovered)}</div>
            )}
          </div>
        </div>
      </div>
    </div>
  )
}
