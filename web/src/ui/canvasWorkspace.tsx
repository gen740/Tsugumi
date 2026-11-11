// The canvas with its floating controls: the project's title (with where the
// canvas is), the run control, Drafts and the project's actions at the top;
// zoom and layout controls at the bottom.
import { ReactNode, useEffect, useRef, useState } from 'react'
import { guide } from '../model/guide'
import { app } from '../model/app'
import { canvasFor } from '../model/canvas'
import { useObserve } from '../model/observable'
import { prefs, ShortcutAction } from '../model/prefs'
import { cardState, formatDuration, key, ProjectSession, runState, runStateStage } from '../model/session'
import { STAGE_INFO } from '../model/canvasKit'
import { Workspace, WorkflowTab } from '../model/workspace'
import { get, str } from '../shared/json'
import { D } from '../shared/rpc'
import { runAll } from './actions'
import { builtinCardItems, GraphCanvas } from './canvas'
import { GuideNest } from './guide'
import { Button, Divider, MenuButton, Popover } from './controls'
import { Icon } from './icons'
import { IssueList } from './workbench'
import { L } from '../i18n'

/** A tooltip that names the shortcut of its action. */
export const tip = (text: string, action: ShortcutAction) => L('{0}（{1}）', text, prefs.label(action))

export function CanvasWorkspace({ workspace, session, tab, onSwitchProject, bottomTrailing }: {
  workspace: Workspace; session: ProjectSession; tab: WorkflowTab
  /** Back to Home (the house button). */
  onSwitchProject?: () => void
  /** Shown at the right of the bottom row (the status bar). */
  bottomTrailing?: ReactNode
}) {
  useObserve(workspace, session, tab)
  return (
    <div className="cv-workspace">
      <GraphCanvas workspace={workspace} session={session} tab={tab} />
      {/* One row so the controls never overlap; the run control stays centered when there is room. */}
      <div className="cv-top">
        <Breadcrumbs session={session} tab={tab} />
        <span className="cv-spacer" />
        <CanvasRunControl workspace={workspace} session={session} tab={tab} />
        <span className="cv-spacer" />
        <CanvasDraftControls workspace={workspace} session={session} tab={tab} />
        <CanvasCommentControls workspace={workspace} session={session} tab={tab} />
        <ProjectActions workspace={workspace} session={session} onSwitchProject={onSwitchProject} />
      </div>
      <div className="cv-bottom">
        <ViewControls session={session} tab={tab} />
        <span className="cv-spacer" />
        {bottomTrailing}
      </div>
    </div>
  )
}

/** Back to Home (as the menu does, with its refusal while a run is on), chat: beside the run control, the same look. */
export function ProjectActions({ workspace, session, onSwitchProject }: { workspace: Workspace; session: ProjectSession; onSwitchProject?: () => void }) {
  useObserve(guide)
  return (
    <div className="capsule material cv-actions">
      {onSwitchProject && <Button kind="borderless" iconOnly icon="house" guide="home" onClick={onSwitchProject} help={tip(L('Home へ戻る'), 'home')} />}
      {/* Another project beside this one: each browser tab has its own. */}
      {onSwitchProject && <Button kind="borderless" iconOnly icon="plus.square" onClick={() => window.open(location.pathname, '_blank')} help={L('新しいタブで Home を開く')} />}
      {!workspace.showChat && onSwitchProject && guide.present ? (
        // While the chat is closed, Tsugumi waits on its button.
        <button type="button" className="btn btn-borderless btn-icon guide-nest-button" data-guide="chat" title={tip(L('チャットを開く'), 'toggleChat')}
          aria-label={tip(L('チャットを開く'), 'toggleChat')} onClick={() => workspace.set('showChat', true)}>
          <GuideNest busy={app.chat(session).busy} small />
        </button>
      ) : (
        <Button kind="borderless" iconOnly icon={workspace.showChat ? 'bubble.left.and.text.bubble.right.fill' : 'bubble.left.and.text.bubble.right'}
          pressed={workspace.showChat} guide="chat"
          help={workspace.showChat ? tip(L('チャットを閉じる'), 'toggleChat') : tip(L('チャットを開く'), 'toggleChat')}
          onClick={() => workspace.set('showChat', !workspace.showChat)} />
      )}
    </div>
  )
}

/** The project's title and where the canvas is (Card in Card), in one place. */
function Breadcrumbs({ session, tab }: { session: ProjectSession; tab: WorkflowTab }) {
  const goUp = () => {
    if (tab.back.length) tab.goBack()
    else if (tab.scope.length) tab.enter(tab.scope.slice(0, -1))
  }
  return (
    <div className="capsule material cv-breadcrumbs">
      {!(tab.scope.length === 0 && tab.back.length === 0) && <Button kind="borderless" small iconOnly icon="chevron.backward" help={L('戻る')} onClick={goUp} />}
      <button type="button" className="cv-crumb-root" title={session.dir} onClick={() => tab.enter([])}>{session.scopeName([])}</button>
      {tab.scope.map((_, i) => (
        <span key={i} className="cv-crumb">
          <Icon name="chevron.right" size={9} className="secondary" />
          <button type="button" className="link cv-ellipsis" onClick={() => tab.enter(tab.scope.slice(0, i + 1))}>{session.scopeName(tab.scope.slice(0, i + 1))}</button>
        </span>
      ))}
    </div>
  )
}

/** Run / stop, the run's state and what it is doing now, elapsed time and the
 *  issue list. Per-Card counts and details are on the Cards, in the history
 *  and on the timeline (a share of finished Cards says nothing about a run
 *  with loops, so there is no progress bar). */
export function CanvasRunControl({ workspace, session, tab }: { workspace: Workspace; session: ProjectSession; tab: WorkflowTab }) {
  useObserve(session, prefs)
  const [showIssues, setShowIssues] = useState(false)
  const issues = useRef<HTMLSpanElement>(null)
  // Shown as running from the start, and for a moment after a quick run, so
  // a short run reads as one step instead of a flicker.
  const [shownActive, setShownActive] = useState(session.runActive)
  const since = useRef(0)
  const [, tick] = useState(0)
  useEffect(() => {
    if (session.runActive) {
      setShownActive(true)
      since.current = Date.now()
      return
    }
    const t = setTimeout(() => !session.runActive && setShownActive(false), Math.max(0, 800 - (Date.now() - since.current)))
    return () => clearTimeout(t)
  }, [session.runActive])
  useEffect(() => {
    if (!shownActive) return
    // The clock ticks in hundredths while the run is shown active.
    const t = setInterval(() => tick((x) => x + 1), 30)
    return () => clearInterval(t)
  }, [shownActive])

  const st = session.runStatus?.state ?? D.RunState.idle
  const info = runState(shownActive ? D.RunState.running : st)
  const label = shownActive && !session.runActive ? runState(D.RunState.running).label : runState(st).label
  const problems = session.diagnostics.filter((d) => d.error).length + [...session.cardStates.values()].filter((s) => s.state === D.CardState.failed).length

  // What the run is doing now: the stage of a running Card, or how many run.
  let activity: string | undefined
  const running = [...session.cardStates].filter(([, s]) => s.state === D.CardState.running || s.state === D.CardState.preparing)
  if (running.length === 1) {
    const [k, s] = running[0]
    const stage = runStateStage(s)
    activity = session.displayName(k) + ' ' + (stage !== undefined ? STAGE_INFO[stage].active : cardState(s.state).label)
  } else if (running.length > 1) activity = L('{0} 枚が実行中', running.length)
  else if (session.runStatus?.state === D.RunState.preparing) activity = L('実行環境を準備中')

  // Run time on the core's clock from acceptance: the same measure while it runs and once it ended.
  let elapsed = ''
  const rs = session.runStatus
  const start = rs?.acceptedAt ?? rs?.startedAt
  if (rs && start !== null && start !== undefined) {
    const end = rs.endedAt !== null ? Number(rs.endedAt) : session.runActive ? session.coreNow() : Number(start)
    elapsed = formatDuration(Math.max(0, end - Number(start)) / 1000)
  }

  return (
    <div className="capsule material cv-run">
      {/* One button of a fixed size: its title and color change, nothing moves. */}
      <Button kind={shownActive ? 'danger' : 'prominent'} icon={shownActive ? 'stop.fill' : 'play.fill'} className="cv-run-button" guide="run"
        disabled={shownActive ? !session.runActive || st === D.RunState.cancelling : !session.connected}
        help={shownActive ? tip(L('Workflow の実行を停止（チャットの回答停止とは別の操作です）'), 'stop') : tip(L('文書の Workflow 全体を開始 Card から実行（未保存の変更は先に保存します）'), 'run')}
        onClick={() => (session.runActive ? void session.cancelRun() : runAll(session))}>
        {shownActive ? L('停止') : L('実行')}
      </Button>
      {/* State and time on one line in a fixed width (what runs now is in the tooltip). */}
      <span className="cv-run-state"
        title={shownActive ? (activity ?? runState(st).label) + L('（経過時間は Run の受付から、実行環境の時計で計ります）') : L('Run の状態と経過時間')}>
        <Icon name={info.icon} style={{ color: `var(--${info.color})` }} />
        <span className="nowrap">{label}</span>
        <span className="secondary mono-digits nowrap">{elapsed}</span>
      </span>
      <span ref={issues}>
        <Button kind="borderless" small icon={problems > 0 ? 'exclamationmark.triangle.fill' : 'checkmark.seal'}
          style={{ color: problems > 0 ? 'var(--orange)' : 'var(--secondary)' }}
          help={L('問題 {0} 件（一覧から対象の Card やログへ移動できます）', problems)} onClick={() => setShowIssues((v) => !v)}>
          <span className="mono-digits">{problems}</span>
        </Button>
      </span>
      <Popover open={showIssues} onClose={() => setShowIssues(false)} anchor={issues} width={460} height={320}>
        <IssueList workspace={workspace} session={session} tab={tab} onDone={() => setShowIssues(false)} />
      </Popover>
      <Divider vertical />
      <MenuButton icon="plus" disabled={!session.isEditable} help={L('組み込みの Card を追加（入力値・スイッチ・Draft・開始・分配・分岐・合流・記録・処理）')}
        items={() => builtinCardItems((t) => {
          const k = canvasFor(session, tab).addBuiltin(t)
          if (k && t === 'draft') canvasFor(session, tab).writeDraft(k)
        })} />
    </div>
  )
}

/** Shown when the current scope has Draft Cards: generate them with the chat. */
export function CanvasDraftControls({ workspace, session, tab }: { workspace: Workspace; session: ProjectSession; tab: WorkflowTab }) {
  const chat = app.chat(session)
  useObserve(session, chat)
  const drafts = session.cards(tab.scope).map((c) => str(get(c, 'id'))).filter((x): x is string => !!x)
    .map((id) => key([...tab.scope, id])).filter((k) => session.kind(k) === 'draft')
  if (!drafts.length) return null
  return (
    <div className="capsule material cv-drafts">
      <Button icon="wand.and.stars" small guide="generate" disabled={session.dirty || !session.connected || session.runActive || chat.busy}
        help={session.dirty ? L('先に変更を保存してください（配線と Draft の内容を確定してから生成します）')
          : L('この階層の Draft をチャットで実装します。進捗はチャットに表示されます。')}
        onClick={() => {
          workspace.set('showChat', true)
          chat.generateDrafts(tab.scope, drafts)
        }}>
        {L('Card を生成')}
      </Button>
    </div>
  )
}

/** Shown when Cards of the current scope have open comments: the selected ones
 *  (or all of them) go to the chat. */
export function CanvasCommentControls({ workspace, session, tab }: { workspace: Workspace; session: ProjectSession; tab: WorkflowTab }) {
  const chat = app.chat(session)
  useObserve(session, chat, tab)
  const cards = session.cardsWithOpenComments(tab.scope)
  if (!cards.length) return null
  const chosen = cards.filter((k) => tab.selection.has(k))
  const asked = chosen.length ? chosen : cards
  const count = asked.reduce((n, k) => n + session.openComments(k).length, 0)
  return (
    <div className="capsule material cv-drafts">
      <Button icon="bubble.left" small guide="from_comments" disabled={session.dirty || !session.connected || chat.busy}
        help={session.dirty ? L('先に変更を保存してください')
          : L('{0}の未解決のコメントに従って、チャットで Card を書き換えます（コメントが求めれば Card を足します）。進捗はチャットに表示されます。', chosen.length ? L('選択した Card') : L('この階層の Card'))}
        onClick={() => {
          workspace.set('showChat', true)
          chat.generateFromComments(tab.scope, asked)
        }}>
        {L('コメントから生成（{0}）', count)}
      </Button>
    </div>
  )
}

/** Zoom and layout controls, floating at the bottom left. */
function ViewControls({ session, tab }: { session: ProjectSession; tab: WorkflowTab }) {
  const cv = canvasFor(session, tab)
  useObserve(cv, prefs, tab)
  const zoom = cv.camera.zoom
  return (
    <div className="capsule material cv-view-controls" data-guide="view">
      <Button kind="borderless" small iconOnly icon="minus.magnifyingglass" help={tip(L('縮小'), 'zoomOut')} onClick={() => cv.zoomFromControls(zoom / 1.2)} />
      <Button kind="borderless" small help={tip(L('100% に戻す'), 'actualSize')} onClick={() => cv.zoomFromControls(1)}>
        <span className="mono-digits" style={{ display: 'inline-block', minWidth: 34, textAlign: 'center' }}>{Math.round(zoom * 100)}%</span>
      </Button>
      <Button kind="borderless" small iconOnly icon="plus.magnifyingglass" help={tip(L('拡大'), 'zoomIn')} onClick={() => cv.zoomFromControls(zoom * 1.2)} />
      <Divider vertical />
      <Button kind="borderless" small iconOnly icon="arrow.up.left.and.down.right.magnifyingglass" help={tip(L('全体表示'), 'fitAll')} onClick={() => cv.fitAll()} />
      {/* Notes, groups and folding are in the canvas's context menu and the menu, where they are named. */}
      <Button kind="borderless" small iconOnly icon="rectangle.grid.2x2" help={L('整列（配置だけを変更）')} onClick={() => session.arrange(tab.scope)} />
      <Button kind="borderless" small iconOnly icon="arrow.triangle.swap" help={L('配線の交差を減らす（Card の位置は保持）')} onClick={() => session.untangle(tab.scope)} />
      <Divider vertical />
      <Button kind="borderless" small iconOnly icon="grid" pressed={prefs.showGrid} help={L('グリッド')} onClick={() => prefs.set('showGrid', !prefs.showGrid)} />
    </div>
  )
}
