// Bottom panel: the whole run log, the run's Cards on a time axis, or a
// terminal in the project's folder. A Card's log is shown by stage.
import { useEffect, useLayoutEffect, useRef, useState } from 'react'
import { chatFollowsAfterScroll } from '../model/chatFollow'
import { STAGE_INFO, Stage } from '../model/canvasKit'
import { useObserve } from '../model/observable'
import { prefs } from '../model/prefs'
import { terminalText } from '../model/terminalText'
import { CardKey, cardState, errorText, formatDuration, formatMillis, key, path, ProjectSession, runState, StageRecord, stageRecords, TimelineEntry } from '../model/session'
import { BOTTOM_ICONS, BOTTOM_TITLES, Workspace, WorkflowTab } from '../model/workspace'
import { D, R, RPCFailure, s } from '../shared/rpc'
import { Button, copyText, download, Empty, Segmented, Select, Sheet, Spinner, Toggle } from './controls'
import { RunComparisonView } from './runComparison'
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

/** Reads one stream of a run (or of one invocation of a Card) from offset 0. */
async function readAll(session: ProjectSession, runId: string, card: string[] | null, invocation: bigint | null, stream: D.LogStream): Promise<string> {
  const rpc = session.rpc
  if (!rpc) return ''
  let text = '', offset = 0n
  for (;;) {
    const slice = await rpc.logRange(runId, card, invocation, stream, offset, false, 256 * 1024)
    text += s(slice.text)
    const next = slice.nextOffset ?? offset
    if (next === offset || next >= (slice.sizeBytes ?? 0n)) break
    offset = next
  }
  return text
}

/** Log text of a run (or one Card / invocation of it), read incrementally
 *  with LogRange from its own next_offset when LogAvailable arrives. A Card's
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
  useObserve(session, prefs)
  const [ownStream, setOwnStream] = useState<D.LogStream>(card ? D.LogStream.stdout : D.LogStream.journal)
  const stream = shownStream ?? ownStream
  const setStream = (v: D.LogStream) => (onStream ? onStream(v) : setOwnStream(v))
  const [log, setLog] = useState('')
  const [error, setError] = useState<string>()
  const [follow, setFollow] = useState(true)
  const state = useRef({ offset: 0n, identity: '', busy: false, again: false })
  const end = useRef<HTMLDivElement>(null)
  const identity = `${runId}|${stream}|${card?.join('/') ?? ''}|${invocation ?? 0}`

  const readMore = async () => {
    const rpc = session.rpc
    const st = state.current
    if (!rpc) return
    if (st.busy) {
      st.again = true
      return
    }
    st.busy = true
    const scope = st.identity
    try {
      let inv = invocation ?? null
      if (card && inv === null) {
        if (runId === session.currentRunId) inv = session.cardStates.get(key(card))?.invocation ?? null
        if (inv === null) inv = (await rpc.record(runId, card).catch(() => undefined))?.selectedInvocation ?? null
      }
      for (;;) {
        const slice = await rpc.logRange(runId, card ?? null, card ? inv : null, stream, st.offset, false, 256 * 1024)
        if (scope !== st.identity) return // the selection changed meanwhile
        if (slice.text) setLog((l) => l + s(slice.text))
        const next = slice.nextOffset ?? st.offset
        if (next === st.offset) break
        st.offset = next
        if (next >= (slice.sizeBytes ?? 0n)) break
      }
      setError(undefined)
    } catch (e) {
      if (!(e instanceof RPCFailure && e.kind === D.FailureKind.not_found)) setError(errorText(e))
    } finally {
      st.busy = false
      if (st.again) {
        st.again = false
        void readMore()
      }
    }
  }

  useEffect(() => {
    state.current = { offset: 0n, identity, busy: false, again: false }
    setLog('')
    void readMore()
  }, [identity])
  useEffect(() => {
    void readMore()
  }, [session.logRevision])
  useEffect(() => {
    if (follow) end.current?.scrollIntoView({ block: 'end' })
  }, [log, follow])

  /** Why nothing is shown, so an empty pane is never ambiguous. */
  const emptyReason = (() => {
    if (error) return L('ログを読み込めませんでした: ') + error
    if (card && runId === session.currentRunId && !session.cardStates.has(key(card))) return L('この実行では、この Card はまだ動いていません。')
    switch (stream) {
      case D.LogStream.stderr: return L('stderr への出力はありません（Nix 評価・build のログもここに出ます）。')
      case D.LogStream.journal: return L('記録された段階の出来事はありません。')
      default: return L('stdout への出力はありません。')
    }
  })()

  return (
    <div className="log-view">
      <div className="panel-bar">
        <Segmented<D.LogStream> small value={stream} onChange={setStream} width={240}
          options={[{ value: D.LogStream.stdout, label: 'stdout' }, { value: D.LogStream.stderr, label: 'stderr' }, { value: D.LogStream.journal, label: 'journal' }]} />
        <span className="caption secondary">{card ? `Card ${session.displayPath(card)}${invocation !== undefined ? ` #${invocation}` : ''}` : L('実行全体')}</span>
        <span className="spacer" />
        <Toggle kind="checkbox" small checked={follow} onChange={setFollow} label={L('末尾を追う')} />
        <Button small disabled={!log} onClick={() => void copyText(log)}>{L('コピー')}</Button>
        <Button small disabled={!log} onClick={() => download(`${runId}-${D.LogStream[stream]}.log`, log, 'text/plain')}>{L('書き出す…')}</Button>
      </div>
      <div className="log-text">
        <pre className={`selectable${log ? '' : ' secondary'}`} style={{ fontSize: prefs.editorFontSize - 1 }}>{terminalText(log) || emptyReason}</pre>
        <div ref={end} />
      </div>
      {error && <div className="caption" style={{ color: 'var(--red)', padding: '0 6px' }}>{error}</div>}
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
 *  each with its state, duration and what it printed, folded unless it failed
 *  or was asked for. The raw streams stay available under "By stream". */
export function StageLogView({ session, runId, card, invocation, focus }: {
  session: ProjectSession; runId: string; card: string[]; invocation?: bigint; focus?: Stage
}) {
  useObserve(session, prefs)
  const k = key(card)
  const [stages, setStages] = useState<StageRecord[]>([])
  const [state, setState] = useState<D.CardState>()
  const [failure, setFailure] = useState<string>()
  const [shownInvocation, setShownInvocation] = useState<bigint>()
  const [logs, setLogs] = useState<Map<D.LogStream, string>>(new Map())
  const [open, setOpen] = useState<Set<string>>(new Set())
  const [rawStream, setRawStream] = useState<D.LogStream>(D.LogStream.stdout)
  const [error, setError] = useState<string>()
  const [loaded, setLoaded] = useState(false)
  const busy = useRef({ running: false, again: false })
  // The log opens at its end and follows it, as the chat does, until the
  // reader scrolls up; back at the end, it follows again.
  const scroll = useRef<HTMLDivElement>(null)
  const following = useRef(true)
  /** Groups opened when they started running: each once, so one the reader folds stays folded. */
  const openedRunning = useRef(new Set<string>())
  const lastTop = useRef(0)

  const code = session.kind(k) === 'code'
  const groups: StageGroup[] = [
    ...(code ? [{ id: 'build' as const, title: L('SDK 準備・Nix 評価・build'), stages: [Stage.prepare, Stage.evaluate, Stage.build] }] : []),
    { id: 'execute', title: L('実行'), stages: [Stage.execute] },
    { id: 'save', title: L('出力検証・保存'), stages: [Stage.validate, Stage.save] },
  ]
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

  const load = async () => {
    const b = busy.current
    if (b.running) {
      b.again = true
      return
    }
    b.running = true
    try {
      const rpc = session.rpc
      if (!rpc) {
        setError(L('実行環境に接続していません'))
        return
      }
      let inv: bigint | null = invocation ?? null
      if (inv === null && runId === session.currentRunId) inv = session.cardStates.get(k)?.invocation ?? null
      const rec = await rpc.record(runId, card, inv, inv === null).catch(() => undefined)
      inv = inv ?? rec?.selectedInvocation ?? null
      setShownInvocation(inv ?? undefined)
      let st: StageRecord[] = []
      const live = session.cardStates.get(k)
      if (runId === session.currentRunId && live && live.invocation === inv) {
        st = live.stages
        setState(live.state)
        setFailure(live.failure)
      } else if (rec?.record?.summary) {
        const sum = rec.record.summary
        st = stageRecords(sum.stages)
        setState(sum.state)
        setFailure(sum.failure?.diagnostic ? s(sum.failure.diagnostic) : undefined)
      }
      setStages(st)
      const out = new Map<D.LogStream, string>()
      for (const stream of [D.LogStream.build, D.LogStream.stdout, D.LogStream.stderr, D.LogStream.journal]) {
        out.set(stream, await readAll(session, runId, card, inv, stream))
      }
      setLogs(out)
      setError(undefined)
      // A failed stage opens by itself; so does the one asked for.
      setOpen((o) => {
        if (o.size) return o
        const n = new Set<string>()
        const failedIn = (g: StageGroup) => st.some((r) => g.stages.includes(r.stage) && r.outcome === D.StageOutcome.failed)
        for (const g of groups) if (failedIn(g)) n.add(g.id)
        const f = focus !== undefined ? groups.find((g) => g.stages.includes(focus)) : undefined
        if (f) n.add(f.id)
        return n
      })
      // So does the one running now, to show its output as it comes.
      const running = groups.filter((g) => st.some((r) => g.stages.includes(r.stage) && r.outcome === D.StageOutcome.running) && !openedRunning.current.has(g.id))
      if (running.length) {
        for (const g of running) openedRunning.current.add(g.id)
        setOpen((o) => new Set([...o, ...running.map((g) => g.id)]))
      }
    } catch (e) {
      if (e instanceof RPCFailure && e.kind === D.FailureKind.not_found) {
        setLogs(new Map())
        setError(undefined)
      } else setError(errorText(e))
    } finally {
      setLoaded(true)
      b.running = false
      if (b.again) {
        b.again = false
        void load()
      }
    }
  }

  useEffect(() => {
    setOpen(new Set())
    setLoaded(false)
    following.current = true
    openedRunning.current = new Set()
    void load()
  }, [runId, k, invocation])
  useLayoutEffect(() => {
    const el = scroll.current
    if (el && following.current) el.scrollTop = el.scrollHeight
  })
  const scrolled = (el: HTMLDivElement) => {
    following.current = chatFollowsAfterScroll(following.current, lastTop.current, { top: el.scrollTop, height: el.scrollHeight, viewport: el.clientHeight })
    lastTop.current = el.scrollTop
  }
  useEffect(() => {
    void load()
  }, [session.logRevision])
  useEffect(() => {
    const g = focus !== undefined ? groups.find((g) => g.stages.includes(focus)) : undefined
    if (g) setOpen((o) => new Set([...o, g.id]))
  }, [focus])

  const toggle = (id: string) => setOpen((o) => {
    const n = new Set(o)
    if (n.has(id)) n.delete(id)
    else n.add(id)
    return n
  })

  /** Journal lines (JSON) of the given phases, as "event: message". */
  const journal = (phases: string[]) => (logs.get(D.LogStream.journal) ?? '').split('\n').flatMap((line) => {
    try {
      const v = JSON.parse(line)
      return typeof v?.phase === 'string' && phases.includes(v.phase) ? [`${v.phase} · ${v.event ?? ''}: ${v.message ?? ''}`] : []
    } catch {
      return []
    }
  }).join('\n')

  const logText = (text: string, empty: string) => (
    <pre className={`stage-log-text selectable${text ? '' : ' empty'}`} style={{ fontSize: prefs.editorFontSize - 1 }}>{terminalText(text) || empty}</pre>
  )

  const shown = shownInvocation ?? invocation ?? 0n
  let body: JSX.Element
  if (!loaded) body = <div className="center-fill"><Spinner /></div>
  else if (error) {
    body = (
      <Empty title={L('ログを読み込めませんでした')} icon="exclamationmark.triangle" description={error}>
        <Button onClick={() => void load()}>{L('再試行')}</Button>
      </Empty>
    )
  } else if (!stages.length && [...logs.values()].every((t) => !t)) {
    body = <Empty title={L('この実行では、この Card はまだ動いていません')} icon="circle.dotted" />
  } else {
    body = (
      <div className="stage-log-scroll" ref={scroll} onScroll={(e) => scrolled(e.currentTarget)}>
        {groups.map((g) => {
          const st = status(g)
          const sec = seconds(g)
          return (
            <div key={g.id} className="stage-group">
              <button type="button" className="stage-group-head" aria-expanded={open.has(g.id)} onClick={() => toggle(g.id)}>
                <Icon name={open.has(g.id) ? 'chevron.down' : 'chevron.right'} size={9} className="secondary" />
                <Icon name={st.icon} style={{ color: `var(--${st.color})` }} />
                <span className="headline callout">{g.title}</span>
                <span className="caption secondary">{st.label}</span>
                <span className="spacer" />
                {sec !== undefined && <span className="caption secondary mono-digits">{formatDuration(sec)}</span>}
              </button>
              {open.has(g.id) && (
                <div className="stage-group-body">
                  <div className="row gap12 wrap">
                    {g.stages.map((sg) => {
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
                  {g.id === 'build' && logText(logs.get(D.LogStream.build) ?? '', L('準備・評価・build の出力はありません（キャッシュや再利用のときは空です）'))}
                  {g.id === 'execute' && (
                    <>
                      <div className="caption headline secondary">stdout</div>
                      {logText(logs.get(D.LogStream.stdout) ?? '', L('stdout への出力はありません'))}
                      <div className="caption headline secondary">stderr</div>
                      {logText(logs.get(D.LogStream.stderr) ?? '', L('stderr への出力はありません'))}
                    </>
                  )}
                  {g.id === 'save' && (
                    <>
                      {logText(journal(['validate', 'save']), L('記録された出来事はありません'))}
                      {failure && st.label === L('失敗') && <div className="caption selectable" style={{ color: 'var(--red)' }}>{failure}</div>}
                    </>
                  )}
                </div>
              )}
            </div>
          )
        })}
        <div className="stage-group">
          <button type="button" className="stage-group-head" aria-expanded={open.has('raw')} onClick={() => toggle('raw')}>
            <Icon name={open.has('raw') ? 'chevron.down' : 'chevron.right'} size={9} className="secondary" />
            <Icon name="list.bullet.rectangle" />
            <span className="callout">{L('stream 別（元の出力）')}</span>
          </button>
          {open.has('raw') && (
            <div className="stage-group-body">
              <Segmented<D.LogStream> small value={rawStream} onChange={setRawStream} width={300}
                options={[{ value: D.LogStream.stdout, label: 'stdout' }, { value: D.LogStream.stderr, label: 'stderr' }, { value: D.LogStream.build, label: 'build' }, { value: D.LogStream.journal, label: 'journal' }]} />
              {logText(logs.get(rawStream) ?? '', L('この stream への出力はありません'))}
            </div>
          )}
        </div>
      </div>
    )
  }
  return (
    <div className="stage-log">
      <div className="panel-bar">
        <span className="headline callout ellipsis">
          {session.displayName(k)} · 実行 #{String(shown)}{state !== undefined ? ` · ${cardState(state).label}` : ''}
        </span>
        <span className="spacer" />
        <Button small onClick={() => setOpen(new Set([...groups.map((g) => g.id), 'raw']))}>{L('すべて開く')}</Button>
        <Button small onClick={() => setOpen(new Set())}>{L('すべて閉じる')}</Button>
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
  })
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
