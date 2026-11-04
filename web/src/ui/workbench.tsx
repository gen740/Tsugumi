// Tabs of one project (the root workflow and opened child workflows) and the
// workbench of the active tab: the sidebar, the canvas with the log/timeline/
// terminal panel under it, and the chat beside it. While the environment
// starts the loading card covers the window; when it stopped or failed a
// problem page takes the canvas's place.
import { ReactNode, useEffect, useRef, useState } from 'react'
import { app } from '../model/app'
import { useObserve } from '../model/observable'
import { prefs, ShortcutAction } from '../model/prefs'
import { CardKey, cardState, key, path, ProjectSession, runState } from '../model/session'
import { canvasFor } from '../model/canvas'
import { BOTTOM_ICONS, BOTTOM_PANELS, BOTTOM_TITLES, BottomPanel, Workspace, WorkflowTab } from '../model/workspace'
import { D, s as text } from '../shared/rpc'
import { CanvasWorkspace } from './canvasWorkspace'
import { ChatPanel } from './chat'
import { Button, Confirm, copyText, Empty, Popover, Spinner, Value } from './controls'
import { LoadingContent, TsugumiLoadingCard } from './home'
import { Icon } from './icons'
import { LogTimelinePanel } from './logs'
import { WorkbenchSidebar } from './sidebar'

const readWidth = (k: string, fallback: number) => {
  try {
    const v = Number(localStorage.getItem(k))
    return Number.isFinite(v) && v > 0 ? v : fallback
  } catch {
    return fallback
  }
}

/** A column beside the content with a draggable edge (the sidebar, the chat). */
function SidePane({ side, storageKey, initial, min, max, className, children }: {
  side: 'left' | 'right'; storageKey: string; initial: number; min: number; max: number; className?: string; children: ReactNode
}) {
  const [width, setWidth] = useState(() => Math.min(max, Math.max(min, readWidth(storageKey, initial))))
  const down = (e: React.MouseEvent) => {
    e.preventDefault()
    const x0 = e.clientX, w0 = width
    const move = (m: MouseEvent) => {
      const w = Math.min(max, Math.max(min, w0 + (side === 'left' ? 1 : -1) * (m.clientX - x0)))
      setWidth(w)
      try {
        localStorage.setItem(storageKey, String(Math.round(w)))
      } catch {}
    }
    const up = () => {
      window.removeEventListener('mousemove', move)
      window.removeEventListener('mouseup', up)
      document.body.classList.remove('resizing-h')
    }
    document.body.classList.add('resizing-h')
    window.addEventListener('mousemove', move)
    window.addEventListener('mouseup', up)
  }
  return (
    <div className={`side-pane side-${side} ${className ?? ''}`} style={{ width }}>
      {children}
      <div className="side-pane-handle" onMouseDown={down} />
    </div>
  )
}

export function WorkflowTabs({ workspace, session, onSwitchProject }: { workspace: Workspace; session: ProjectSession; onSwitchProject?: () => void }) {
  useObserve(workspace, session)
  const tab = workspace.active
  useEffect(() => {
    document.title = session.scopeName([])
  })
  const problem = hasProblem(session)
  return (
    <div className="window workflow-window">
      {workspace.showSidebar && tab && (
        <SidePane side="left" storageKey="tsugumi.sidebar.width" initial={250} min={200} max={380} className="sidebar-pane">
          <div className="sidebar-top">
            <Button kind="borderless" small iconOnly icon="sidebar.left" help="サイドバーを隠す" onClick={() => workspace.set('showSidebar', false)} />
          </div>
          <WorkbenchSidebar workspace={workspace} session={session} tab={tab} />
        </SidePane>
      )}
      <div className={`workflow-detail${workspace.showSidebar ? '' : ' sidebar-hidden'}`}>
        {!workspace.showSidebar && (
          <Button kind="borderless" iconOnly icon="sidebar.left" className="sidebar-show material" help="サイドバーを表示" onClick={() => workspace.set('showSidebar', true)} />
        )}
        {problem ? (
          // Nothing of the canvas stays behind the problem: it replaces the content.
          <EnvironmentProblemView session={session} onSwitchProject={onSwitchProject} />
        ) : (
          <>
            {workspace.tabs.length > 1 && (
              <div className="tabbar" role="tablist">
                {workspace.tabs.map((t) => <TabButton key={t.id} t={t} active={t.id === tab?.id} session={session} workspace={workspace} />)}
              </div>
            )}
            {tab && <Workbench key={tab.id} workspace={workspace} session={session} tab={tab} />}
          </>
        )}
      </div>
      {!problem && workspace.showChat && (
        // Wider by default; the width the user gives it is kept (per browser).
        <SidePane side="right" storageKey="chat.width" initial={480} min={320} max={900} className="chat-pane">
          <ChatPanel session={session} chat={app.chat(session)} detached={false} tab={tab} />
        </SidePane>
      )}
      <LoadingOverlay content={loadingContent(session)} />
    </div>
  )
}

function TabButton({ t, active, session, workspace }: { t: WorkflowTab; active: boolean; session: ProjectSession; workspace: Workspace }) {
  useObserve(t)
  return (
    <div className={`tab${active ? ' active' : ''}`} role="tab" aria-selected={active} onClick={() => workspace.selectTab(t.id)}>
      <Icon name={t.scope.length === 0 ? 'square.stack.3d.up.fill' : 'square.stack.3d.up'} size={12} />
      <span className="ellipsis">{session.scopeName(t.scope)}</span>
      <Button kind="borderless" small iconOnly icon="xmark" help="タブを閉じる" onClick={() => workspace.closeTab(t.id)} />
    </div>
  )
}

/** Canvas and the log/timeline/terminal panel of one tab (the sidebar and
 *  the chat belong to the window). */
export function Workbench({ workspace, session, tab }: { workspace: Workspace; session: ProjectSession; tab: WorkflowTab }) {
  useObserve(workspace, tab)
  useEffect(() => {
    session.saveViewSoon(tab.scope, tab.selection)
  }, [session, tab, tab.scope, tab.selection])
  return (
    <div className="workbench">
      <div className="workbench-canvas">
        <CanvasWorkspace workspace={workspace} session={session} tab={tab} onSwitchProject={workspace.switchProject}
          bottomTrailing={<StatusBar workspace={workspace} session={session} tab={tab} />} />
      </div>
      {workspace.showBottom && <LogTimelinePanel workspace={workspace} session={session} tab={tab} />}
    </div>
  )
}

// ---- Environment: loading card and problem page ----------------------------------------

/** What the loading card says while the environment starts or the project
 *  opens; undefined once connected or when there is a problem to show instead. */
export function loadingContent(session: ProjectSession): LoadingContent | undefined {
  if (session.connected) return undefined
  const dirty = session.dirty
  const env = session.environment
  switch (env.state) {
    case 'failed':
    case 'stopped':
      return undefined
    case 'reconnecting':
      return { title: '再接続しています…', message: '未保存の変更は保持しています。', dirty }
    case 'starting':
      return { title: '起動しています', message: env.message || 'このプロジェクトの tsugumid を起動しています', detail: 'tsugumid', dirty }
    case 'ready':
      if (session.connectionMessage) return undefined
      return { title: 'プロジェクトを読み込んでいます…', detail: 'project snapshot', dirty }
  }
}

/** Whether the environment or the project cannot be used now (a problem is
 *  shown instead of the canvas). */
export function hasProblem(session: ProjectSession): boolean {
  if (session.connected) return false
  const st = session.environment.state
  if (st === 'failed' || st === 'stopped') return true
  if (st === 'ready') return !!session.connectionMessage
  return false
}

/** The loading card over the window, kept at least 300 ms so it never
 *  flickers, fading in quickly and out a little slower. */
function LoadingOverlay({ content }: { content?: LoadingContent }) {
  const [shown, setShown] = useState(content)
  const [leaving, setLeaving] = useState(false)
  const since = useRef(content ? Date.now() : 0)
  const key = content ? JSON.stringify(content) : ''
  useEffect(() => {
    if (content) {
      if (!shown || leaving) since.current = Date.now()
      setShown(content)
      setLeaving(false)
      return
    }
    if (!shown) return
    const wait = Math.max(0, 300 - (Date.now() - since.current))
    const t1 = setTimeout(() => setLeaving(true), wait)
    const t2 = setTimeout(() => {
      setShown(undefined)
      setLeaving(false)
    }, wait + 200)
    return () => {
      clearTimeout(t1)
      clearTimeout(t2)
    }
  }, [key])
  if (!shown) return null
  return (
    <div className={`loading-overlay${leaving ? ' leaving' : ''}`} style={{ pointerEvents: content ? 'auto' : 'none' }}>
      <TsugumiLoadingCard content={shown} />
    </div>
  )
}

interface Problem {
  icon: string
  color: string
  title: string
  message: string
  recovery?: string
  log: string
  canStop: boolean
  /** The line of the log that says why (tsugumid's last error). */
  cause?: string
}

/** The last error line of tsugumid's log, without its time and level. */
function causeIn(log: string): string | undefined {
  const line = log.split('\n').reverse().find((l) => l.includes(' error '))
  if (!line) return undefined
  const i = line.indexOf(' error ')
  return line.slice(i + ' error '.length).trim()
}

/** The page shown when the environment did not start (or stopped, or the
 *  project could not be read): what happened, what to do, the actions, and
 *  the log for the details. It replaces the canvas. */
function EnvironmentProblemView({ session, onSwitchProject }: { session: ProjectSession; onSwitchProject?: () => void }) {
  useObserve(session)
  const [showLog, setShowLog] = useState(false)
  const [copied, setCopied] = useState(false)
  const env = session.environment
  let p: Problem
  if (env.state === 'failed') {
    const log = env.log ?? ''
    p = { icon: 'xmark.octagon.fill', color: 'red', title: '実行環境を起動できませんでした', message: env.message ?? '', recovery: env.recovery || undefined, log, canStop: true, cause: causeIn(log) }
    if (p.cause?.includes('cannot open the project')) {
      p.recovery = 'プロジェクトのファイル（tsugumi.json・layout.json・definitions の中）の書式を直してから、再試行してください。文書と実行履歴は消えません。'
    }
  } else if (env.state === 'stopped') {
    p = { icon: 'pause.circle.fill', color: 'secondary', title: '実行環境は停止しています', message: 'このプロジェクトの実行環境（tsugumid）は動いていません。起動すると編集と実行ができます。', log: '', canStop: false }
  } else {
    p = { icon: 'exclamationmark.triangle.fill', color: 'orange', title: 'プロジェクトを読み込めませんでした', message: session.connectionMessage ?? '',
      recovery: 'tsugumi.json などの書式が正しいかを確かめて、もう一度試してください。', log: '', canStop: true }
  }
  return (
    <div className="problem-page scroll">
      <div className="problem-body">
        <div className="row center gap12">
          <Icon name={p.icon} size={34} style={{ color: `var(--${p.color})` }} />
          <div className="col gap4 min0">
            <div className="title2">{p.title}</div>
            <div className="callout secondary ellipsis-middle" title={session.dir}>{session.scopeName([])} · {session.dir}</div>
          </div>
        </div>
        {p.message && (
          <ProblemSection title="何が起きたか">
            <div className="selectable wrap-pre">{p.message}</div>
            {p.cause && (
              <div className="col gap4">
                <div className="caption secondary">原因（ログより）</div>
                <div className="callout mono selectable wrap-pre">{p.cause}</div>
              </div>
            )}
          </ProblemSection>
        )}
        {p.recovery && <ProblemSection title="どうすればよいか"><div className="selectable wrap-pre">{p.recovery}</div></ProblemSection>}
        {session.dirty && <span className="label callout" style={{ color: 'var(--orange)' }}><Icon name="pencil.circle" />保存できていない変更は保持しています（起動すると保存します）</span>}
        <div className="row gap8 center">
          <Button kind="prominent" onClick={() => session.start()}>{p.canStop ? '再試行' : '起動'}</Button>
          {p.canStop && <Button onClick={() => void session.stopEnvironment()}>環境を停止</Button>}
          <span className="spacer" />
          {onSwitchProject && <Button onClick={onSwitchProject}>別のプロジェクトを開く…</Button>}
        </div>
        {p.log && (
          <div className="col gap6">
            <button type="button" className="disclosure-head headline callout" aria-expanded={showLog} onClick={() => setShowLog(!showLog)}>
              <Icon name={showLog ? 'chevron.down' : 'chevron.right'} size={9} />詳しいログ
            </button>
            {showLog && (
              <>
                <pre className="problem-log selectable">{p.log.slice(-20000)}</pre>
                <div className="row end">
                  <Button small onClick={() => { void copyText(p.log); setCopied(true) }}>{copied ? 'コピーしました' : 'ログをコピー'}</Button>
                </div>
              </>
            )}
          </div>
        )}
      </div>
    </div>
  )
}

function ProblemSection({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="problem-section">
      <div className="callout headline secondary">{title}</div>
      {children}
    </div>
  )
}

// ---- Status bar --------------------------------------------------------------------

const PANEL_SHORTCUT: Record<BottomPanel, ShortcutAction> = { log: 'toggleLog', timeline: 'toggleTimeline', terminal: 'toggleTerminal' }

/** A status bar control that looks pressable: a light background, stronger
 *  on hover and when pressed (plain status texts have none). */
const StatusButton = ({ selected, help, onClick, children, label, buttonRef }: {
  selected?: boolean; help?: string; onClick: () => void; children: ReactNode; label?: string; buttonRef?: React.Ref<HTMLButtonElement>
}) => (
  <button ref={buttonRef} type="button" className={`status-button${selected ? ' selected' : ''}`} title={help} aria-label={label} aria-pressed={selected}
    onClick={(e) => { e.stopPropagation(); onClick() }}>
    {children}
  </button>
)

/** Floating bar at the canvas's bottom right: run state and save/sync state
 *  shown separately, counts, and the toggles of the log/timeline/terminal
 *  panels. It takes shorter forms when room is short (never overlapping). */
export function StatusBar({ workspace, session, tab }: { workspace: Workspace; session: ProjectSession; tab: WorkflowTab }) {
  useObserve(workspace, session, tab, prefs)
  const [showSync, setShowSync] = useState(false)
  const [showIssues, setShowIssues] = useState(false)
  const [confirmDiscard, setConfirmDiscard] = useState(false)
  const syncRef = useRef<HTMLButtonElement>(null)
  const issueRef = useRef<HTMLButtonElement>(null)
  const st = session.runStatus?.state ?? D.RunState.idle
  const rs = runState(st)
  const failure = session.runStatus?.failure
  const sync = session.syncState
  const errors = session.diagnostics.filter((d) => d.error).length
  const warnings = session.diagnostics.length - errors
  return (
    <div className="statusbar-wrap">
      <div className="statusbar material">
        <span className="label nowrap" title={rs.label}>
          <Icon name={rs.icon} style={{ color: `var(--${rs.color})` }} />
          <span className="sb-long">実行: {rs.label}</span><span className="sb-short">{rs.label}</span>
          {st === D.RunState.failed && failure && (
            <span className="sb-long sb-full secondary ellipsis" style={{ maxWidth: 260 }}> · {failure.diagnostic ? text(failure.diagnostic) : D.FailureKind[failure.kind]}</span>
          )}
        </span>
        <span className="vdivider sb-not-min" />
        <StatusButton buttonRef={syncRef} help="保存・同期の詳細" onClick={() => setShowSync(!showSync)}>
          {sync.kind === 'saved' && <><Icon name="checkmark.icloud" style={{ color: 'var(--green)' }} /><span>保存済み</span></>}
          {sync.kind === 'waitingForRun' && <><Icon name="clock.arrow.circlepath" style={{ color: 'var(--orange)' }} /><span className="sb-long">実行が終わったら保存します</span><span className="sb-short">実行後に保存</span></>}
          {sync.kind === 'saving' && <><Spinner small /><span>保存中…</span></>}
          {sync.kind === 'failed' && <><Icon name="exclamationmark.icloud.fill" style={{ color: 'var(--red)' }} /><span>同期できません</span></>}
          {sync.kind === 'offline' && <><Icon name="icloud.slash" className="secondary" /><span>未接続</span></>}
        </StatusButton>
        {/* The saved revision; the project's versions (git) open from it. */}
        <span className="sb-full">
          <StatusButton label="版の履歴" help={`版の履歴を開く（確定 revision ${String(session.rev)}）`} onClick={() => app.openWindow(session, { kind: 'versions' })}>
            <Icon name="point.3.connected.trianglepath.dotted" /><span className="mono-digits">rev {String(session.rev)}</span>
          </StatusButton>
        </span>
        <Popover open={showSync} onClose={() => setShowSync(false)} anchor={syncRef} width={440}>
          <div className="popover-body">
            <div className="headline">保存・同期</div>
            <Value label="確定 revision" value={String(session.rev)} />
            <Value label="実行 revision" value={String(session.executionRev)} />
            {sync.kind === 'failed' && <div className="selectable wrap-pre" style={{ color: 'var(--red)' }}>{sync.message}</div>}
            <div className="callout secondary">変更は操作するたびに保存されます。チャットや別のウインドウでの変更もすぐに反映されます。</div>
            <div className="row gap8 wrap">
              {session.saveError !== undefined && (
                <>
                  <Button disabled={!session.connected} onClick={() => void session.save()}>もう一度保存</Button>
                  <Button kind="destructive" onClick={() => setConfirmDiscard(true)}>変更を破棄…</Button>
                </>
              )}
              <Button disabled={!session.connected} help="Tsugumi の外で編集されたファイルを取り込みます" onClick={() => void session.reloadExternal()}>外部の変更を再読込</Button>
              <Button icon="point.3.connected.trianglepath.dotted" help="実行ごとに残った版（git）を枝・タグ・実行結果と一緒に見ます"
                onClick={() => { setShowSync(false); app.openWindow(session, { kind: 'versions' }) }}>版の履歴…</Button>
            </div>
          </div>
        </Popover>
        <span className="vdivider sb-full" />
        <span className="row center gap8 sb-full nowrap">
          <StatusButton buttonRef={issueRef} help="問題一覧" onClick={() => setShowIssues(!showIssues)}>
            <span className="label" style={{ color: errors ? 'var(--red)' : 'var(--secondary)' }}><Icon name="xmark.octagon" />{errors}</span>
            <span className="label" style={{ color: warnings ? 'var(--yellow)' : 'var(--secondary)' }}><Icon name="exclamationmark.triangle" />{warnings}</span>
          </StatusButton>
        </span>
        <Popover open={showIssues} onClose={() => setShowIssues(false)} anchor={issueRef} width={460} height={320}>
          <IssueList workspace={workspace} session={session} tab={tab} onDone={() => setShowIssues(false)} />
        </Popover>
        <span className="spacer" />
        {session.notice && (
          <span className="row center gap4 sb-full min0">
            <span className="label secondary ellipsis notice" title={session.notice}><Icon name="info.circle" /><span className="ellipsis">{session.notice}</span></span>
            <Button kind="borderless" small iconOnly icon="xmark" help="閉じる" onClick={() => { session.notice = undefined; session.changed() }} />
          </span>
        )}
        <span className="row gap2 nowrap">
          {BOTTOM_PANELS.map((p) => {
            const open = workspace.showBottom && workspace.bottomPanel === p
            return (
              <StatusButton key={p} selected={open} label={BOTTOM_TITLES[p]} help={`${BOTTOM_TITLES[p]}（${prefs.label(PANEL_SHORTCUT[p])}）`} onClick={() => workspace.togglePanel(p)}>
                <Icon name={BOTTOM_ICONS[p]} />
              </StatusButton>
            )
          })}
        </span>
      </div>
      {confirmDiscard && (
        <Confirm title="保存できない変更を破棄して、保存済みの内容に戻しますか？" message="破棄した変更は取り消せません。"
          confirm="変更を破棄" onCancel={() => setConfirmDiscard(false)} onConfirm={() => session.discardDraft()} />
      )}
    </div>
  )
}

/** Diagnostics and Card failures; a row jumps to its Card or its log. */
export function IssueList({ workspace, session, tab, onDone }: { workspace: Workspace; session: ProjectSession; tab: WorkflowTab; onDone?: () => void }) {
  useObserve(session)
  const issues: { id: string; error: boolean; title: string; detail: string; card?: string[]; run: boolean }[] = []
  for (const [k, c] of [...session.cardStates].sort(([a], [b]) => a.localeCompare(b))) {
    if (c.state === D.CardState.failed || c.state === D.CardState.blocked) {
      issues.push({ id: 'run:' + k, error: c.state === D.CardState.failed, title: `${session.displayName(k)}: ${cardState(c.state).label}`, detail: c.failure ?? '', card: path(k), run: true })
    }
  }
  for (const d of session.diagnostics) {
    issues.push({ id: d.id, error: d.error, title: d.message, detail: [d.card?.join('/'), d.path, d.code].filter(Boolean).join(' · '), card: d.card, run: false })
  }
  if (!issues.length) return <Empty title="問題はありません" icon="checkmark.seal" />
  const reveal = (card: string[]) => {
    tab.enter(card.slice(0, -1))
    tab.setSelection([key(card)])
    canvasFor(session, tab).reveal(key(card))
    onDone?.()
  }
  return (
    <div className="list">
      {issues.map((i) => (
        <div key={i.id} className="list-row issue">
          <Icon name={i.error ? 'xmark.circle.fill' : 'exclamationmark.triangle.fill'} style={{ color: i.error ? 'var(--red)' : 'var(--yellow)' }} />
          <div className="flex1 min0">
            <div className="selectable">{i.title}</div>
            {i.detail && <div className="caption secondary clamp3">{i.detail}</div>}
          </div>
          {i.card && (
            <>
              <Button kind="borderless" small onClick={() => reveal(i.card!)}>表示</Button>
              {i.run && <Button kind="borderless" small onClick={() => { workspace.showLog(key(i.card!)); onDone?.() }}>ログ</Button>}
            </>
          )}
        </div>
      ))}
    </div>
  )
}

export type { CardKey }
