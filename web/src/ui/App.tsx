// The main window: initial setup when the host needs it, then the project
// chooser, then the workbench tabs of the opened project. Keyboard shortcuts
// are routed here to the active project and tab (the macOS menu commands);
// the canvas handles its own single-key shortcuts while it has the focus.
import { useEffect, useRef, useState } from 'react'
import { closePageProject, onServerEvent, reportPageProject } from '../api'
import { app } from '../model/app'
import { guide } from '../model/guide'
import { canvasFor } from '../model/canvas'
import { useObserve } from '../model/observable'
import { eventBinding, isMac, prefs } from '../model/prefs'
import { errorText } from '../model/session'
import { Workspace } from '../model/workspace'
import { runAll } from './actions'
import { Button, Empty, MenuHost, Sheet, Spinner, TextField } from './controls'
import { GuideFlyer, GuideInvite, GuideSetupSheet, setGuideWindow } from './guide'
import { ProjectChooser, SetupView } from './home'
import { FolderPicker } from './sidebar'
import { WindowHost } from './windows'
import { WorkflowTabs } from './workbench'

const isTextInput = (el: Element | null) =>
  !!el && (el instanceof HTMLTextAreaElement || (el instanceof HTMLInputElement && !['checkbox', 'radio', 'range', 'button'].includes(el.type)) ||
    (el as HTMLElement).isContentEditable || el instanceof HTMLSelectElement)

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))

export function App() {
  useObserve(app)
  const [workspace] = useState(() => new Workspace())
  useObserve(workspace)
  const [switchError, setSwitchError] = useState<string>()
  // Leaving was asked for and the changes cannot be saved: the way out is to give them up.
  const [unsaved, setUnsaved] = useState<{ reason: string; answer: (giveUp: boolean) => void }>()
  const [duplicating, setDuplicating] = useState(false)
  const params = new URLSearchParams(location.search)
  const detachedScope = params.get('scope')
  const detached = detachedScope !== null

  const open = async (dir: string, scope?: string[]) => {
    try {
      const s = await app.session(dir)
      workspace.open(s, scope)
      if (scope) workspace.set('showChat', false)
      reportPageProject(s.dir)
      const u = new URL(location.href)
      u.searchParams.set('project', s.dir)
      if (!scope) u.searchParams.delete('scope')
      u.searchParams.delete('fresh')
      history.replaceState(null, '', u)
    } catch (e) {
      app.setError(e)
    }
  }

  useEffect(() => {
    void app.refresh().then(() => {
      void guide.refresh()
      // `?project=<folder>` (or the server's launch argument) opens that project.
      const p = params.get('project') ?? app.status?.launchProject
      if (p && params.has('fresh')) app.markFresh(p)
      if (p && !app.setupNeeded) void open(p, detached ? detachedScope!.split('/').filter(Boolean) : undefined)
      // `?new` is another tab's ⌘N: this tab's Home opens the sheet.
      if (params.has('new')) {
        app.request('requestNewProject', true)
        const u = new URL(location.href)
        u.searchParams.delete('new')
        history.replaceState(null, '', u)
      }
    })
  }, [])

  /** Back to the chooser: allowed only when nothing runs and the needed save succeeded. */
  const leaveProject = async (): Promise<boolean> => {
    const s = workspace.session
    if (!s) return true
    if (s.runActive) {
      setSwitchError('実行中はプロジェクトを切り替えられません。停止するか終了を待ってください。')
      return false
    }
    const settled = await s.settleBeforeLeaving((reason) => new Promise((answer) => setUnsaved({ reason, answer })))
    if (!settled) return false
    workspace.close()
    app.closeProject(s)
    const u = new URL(location.href)
    u.search = ''
    history.replaceState(null, '', u)
    await closePageProject(s.dir).catch(() => {})
    void app.refresh()
    return true
  }
  workspace.switchProject = detached ? undefined : () => void leaveProject()
  // The guide's buttons act on this page.
  setGuideWindow({
    openProject: (p) => void openProject(p),
    home: () => void leaveProject(),
    newProject: () => void newProject(),
    showChat: () => workspace.set('showChat', true),
  })

  /** Opens another project, once the current one has been left (saved first when needed). */
  const openReplacing = async (dir: string) => {
    if (workspace.session && !(await leaveProject())) return
    for (let i = 0; i < 50 && workspace.session; i++) await sleep(100)
    if (!workspace.session) await open(dir)
  }

  /** A project opens on this tab's Home; from a project, in a browser tab of its own
   *  (a browser that blocks the new tab: here, once the open one has been left). */
  const openProject = async (dir: string) => {
    if (!workspace.session) return open(dir)
    if (workspace.session.dir === dir) return
    const q = new URLSearchParams({ project: dir })
    const fresh = app.takeFresh(dir)
    if (fresh) q.set('fresh', '')
    if (window.open(`${location.pathname}?${q}`, '_blank')) return
    if (fresh) app.markFresh(dir)
    await openReplacing(dir)
  }

  /** The new project sheet, on this tab's Home; from a project, on a new tab's. */
  const newProject = async () => {
    if (workspace.session && window.open(`${location.pathname}?new`, '_blank')) return
    if (workspace.session && !(await leaveProject())) return
    app.request('requestNewProject', true)
  }

  // The latest handlers, for the one keydown listener.
  const commands = useRef({ leaveProject, openProject, newProject })
  commands.current = { leaveProject, openProject, newProject }

  // Claude Code (the plugin) opened a project: the server chose this page to
  // show it (on Home; other pages keep their projects). When it asks to widen
  // what Cards may do, the chat (where that is approved) opens.
  useEffect(() => {
    if (detached) {
      // A child workflow's page closes with its project.
      return onServerEvent((e) => {
        const s = workspace.session
        if (s && e.type === 'projectClosed' && s.dir === e.project) {
          workspace.close()
          app.closeProject(s)
          window.close()
        }
      })
    }
    return onServerEvent((e) => {
      if (e.type === 'agentOpen' && !workspace.session) void commands.current.openProject(e.project)
      if (e.type === 'chat' && e.session === 'external' && e.event?.kind === 'permission' && workspace.session?.dir === e.project) workspace.set('showChat', true)
    })
  }, [])

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (document.body.dataset.recording) return
      const b = eventBinding(e)
      const textFocused = isTextInput(document.activeElement)
      if (b.command && !b.option && !b.control && !b.shift && b.key === ',') {
        e.preventDefault()
        app.openWindow(undefined, { kind: 'appSettings' })
        return
      }
      const s = workspace.session
      const tab = workspace.active
      // Undo/redo of the document; text fields keep their own undo.
      if (b.command && !b.option && !b.control && b.key === 'z' && s) {
        if (textFocused) return
        e.preventDefault()
        if (b.shift) s.redo()
        else s.undo()
        return
      }
      if (e.key === 'Escape' && !textFocused) {
        const front = app.frontWindow
        if (front && document.activeElement?.closest(`[data-window="${front.id}"]`)) {
          e.preventDefault()
          app.closeWindow(front.id)
        }
        return
      }
      // Single-key canvas shortcuts are the canvas's own (it handles them while focused).
      const action = prefs.action(e, false)
      if (!action) return
      // ⌘F in a text editor is the editor's own find; elsewhere it searches the canvas.
      if (action === 'find' && textFocused) return
      // Handled where they apply (the canvas's details bubble, the code editor).
      if (action === 'showDetails' || action === 'formatSource') return
      // Taken from the browser too (⌘N, ⌘O, ⌘R, ...): these are the app's commands.
      e.preventDefault()
      e.stopPropagation()
      switch (action) {
        case 'newProject':
          if (!detached) void commands.current.newProject()
          return
        case 'openProject':
          if (!detached) app.request('requestOpenProject', true)
          return
        case 'home':
          if (!detached) void commands.current.leaveProject()
          return
        case 'duplicateProject':
          if (s && !detached) setDuplicating(true)
          return
        case 'toggleGuide':
          // In a project Tsugumi is the chat; on Home, its panel in the corner.
          if (detached || !guide.available) return
          if (workspace.session) workspace.set('showChat', !workspace.showChat)
          else guide.set('open', !guide.open)
          return
      }
      if (!s || !tab) return
      switch (action) {
        case 'find': canvasFor(s, tab).find(); break
        case 'run': if (s.connected && !s.runActive) runAll(s); break
        case 'stop': if (s.runActive) void s.cancelRun(); break
        case 'addNote': if (s.isEditable) canvasFor(s, tab).addNote(); break
        case 'toggleChat': if (!detached) workspace.set('showChat', !workspace.showChat); break
        case 'toggleLog': workspace.togglePanel('log'); break
        case 'toggleTimeline': workspace.togglePanel('timeline'); break
        case 'toggleTerminal': workspace.togglePanel('terminal'); break
      }
    }
    window.addEventListener('keydown', handler, true)
    return () => window.removeEventListener('keydown', handler, true)
  }, [workspace])

  // Edits are saved as they are made; a save still on its way is not lost
  // by closing the browser tab without a warning.
  useEffect(() => {
    const before = (e: BeforeUnloadEvent) => {
      if ([...app.sessions.values()].some((s) => s.dirty)) e.preventDefault()
    }
    window.addEventListener('beforeunload', before)
    return () => window.removeEventListener('beforeunload', before)
  }, [])

  let body: JSX.Element
  if (!app.status) {
    body = app.error ? <Empty title="サーバーに接続できません" icon="network.slash" description={app.error}><Button onClick={() => void app.refresh()}>再試行</Button></Empty>
      : <div className="center-fill"><Spinner /></div>
  } else if (app.setupNeeded) {
    body = <SetupView />
  } else if (workspace.session) {
    body = <WorkflowTabs workspace={workspace} session={workspace.session} onSwitchProject={workspace.switchProject} />
  } else {
    body = <ProjectChooser onOpen={(p) => void open(p)} />
  }
  return (
    <div className={`app${isMac ? ' mac' : ''}`}>
      {body}
      <WindowHost />
      <MenuHost />
      {!detached && <GuideFlyer />}
      {!detached && !app.setupNeeded && app.status && <GuideSetupSheet />}
      {!detached && <GuideInvite />}
      {app.requestOpenProject && (
        <FolderPicker message="tsugumi.json を含むフォルダを選んでください" requireProject
          onClose={() => app.request('requestOpenProject', false)}
          onPick={(p) => {
            app.request('requestOpenProject', false)
            void openProject(p)
          }} />
      )}
      {duplicating && workspace.session && (
        <DuplicateProjectSheet dir={workspace.session.dir} onClose={() => setDuplicating(false)}
          onError={(m) => setSwitchError(m)} onCreated={(p) => void openProject(p)} />
      )}
      {unsaved && (
        <Sheet onClose={() => { setUnsaved(undefined); unsaved.answer(false) }} width={420}>
          <div className="confirm">
            <div className="confirm-title">保存できない変更があります</div>
            <div className="secondary">Home へ戻るには、保存できない変更を破棄します。破棄した変更は取り消せません。</div>
            <div className="secondary selectable">保存できない理由: {unsaved.reason}</div>
            <div className="row end gap8">
              <Button onClick={() => { setUnsaved(undefined); unsaved.answer(false) }}>キャンセル</Button>
              <Button kind="danger" onClick={() => { setUnsaved(undefined); unsaved.answer(true) }}>変更を破棄して Home へ戻る</Button>
            </div>
          </div>
        </Sheet>
      )}
      {switchError && (
        <Sheet onClose={() => setSwitchError(undefined)} width={420}>
          <div className="confirm">
            <div className="confirm-title">プロジェクトを切り替えられません</div>
            <div className="secondary selectable">{switchError}</div>
            <div className="row end"><Button kind="prominent" onClick={() => setSwitchError(undefined)}>OK</Button></div>
          </div>
        </Sheet>
      )}
    </div>
  )
}

/** A copy of the open project (its saved documents and sources), then opened. */
function DuplicateProjectSheet({ dir, onClose, onError, onCreated }: { dir: string; onClose: () => void; onError: (m: string) => void; onCreated: (path: string) => void }) {
  const folder = dir.split('/').filter(Boolean).pop() ?? 'project'
  const [name, setName] = useState(`${folder} copy`)
  const [choosing, setChoosing] = useState(false)
  const parent = dir.split('/').slice(0, -1).join('/') || '/'
  return (
    <Sheet onClose={onClose} width={420}>
      <form className="sheet-body" onSubmit={(e) => { e.preventDefault(); if (name.trim()) setChoosing(true) }}>
        <div className="headline">複製を作る場所を選んでください（保存済みの内容を複製します）</div>
        <TextField value={name} onChange={setName} placeholder="名前" autoFocus />
        <div className="row end gap8">
          <Button onClick={onClose}>キャンセル</Button>
          <Button kind="prominent" type="submit" disabled={!name.trim()}>場所を選ぶ…</Button>
        </div>
      </form>
      {choosing && (
        <FolderPicker message="複製を作る場所を選んでください（保存済みの内容を複製します）" allowCreate initial={parent}
          onClose={() => setChoosing(false)}
          onPick={async (p) => {
            setChoosing(false)
            try {
              const made = await app.duplicateProject(dir, p, name.trim())
              onClose()
              onCreated(made)
            } catch (e) {
              onClose()
              onError('複製できませんでした: ' + errorText(e))
            }
          }} />
      )}
    </Sheet>
  )
}
