// Detail windows (Card settings, history, artifacts, ...) floating inside the
// page: movable, resizable, stacked; each one shows one project's session.
import { app, DetailWindow } from '../model/app'
import { useObserve } from '../model/observable'
import { CardSettingsEditor } from './cardSettings'
import { ChatPanel } from './chat'
import { CardArtifactsView, CardExecutionView, VisualizationWindow } from './history'
import { Icon } from './icons'
import { CardDatabaseView, DraftEditor, WorkflowSettingsView } from './projectViews'
import { ApplicationSettings } from './settings'

function title(w: DetailWindow): string {
  const s = w.session
  const name = (k: string) => s?.displayName(k) ?? k
  switch (w.what.kind) {
    case 'card': return `Card 設定 · ${name(w.what.card)}`
    case 'draft': return `Draft · ${name(w.what.card)}`
    case 'definitions': return 'Card 定義'
    case 'executions': return `実行履歴 · ${name(w.what.card)}`
    case 'artifacts': return `成果物 · ${name(w.what.card)}`
    case 'workflowSettings': return `Workflow 設定 · ${s?.scopeName([]) ?? ''}`
    case 'visual': return `可視化 · ${name(w.what.card)}`
    case 'chat': return 'チャット'
    case 'appSettings': return '設定'
  }
}

function Content({ w }: { w: DetailWindow }) {
  const s = w.session
  const close = () => app.closeWindow(w.id)
  const what = w.what
  if (what.kind === 'appSettings') return <ApplicationSettings />
  if (!s) return null
  switch (what.kind) {
    case 'card': return <CardSettingsEditor session={s} card={what.card} initial={what.category} onClose={close} />
    case 'draft': return <DraftEditor session={s} cardKey={what.card} onClose={close} />
    case 'definitions': return <CardDatabaseView session={s} definition={what.definition} />
    case 'executions': return <CardExecutionView session={s} cardKey={what.card} />
    case 'artifacts': return <CardArtifactsView session={s} cardKey={what.card} />
    case 'workflowSettings': return <WorkflowSettingsView session={s} />
    case 'visual': return <VisualizationWindow session={s} cardKey={what.card} />
    case 'chat': return <ChatPanel session={s} chat={app.chat(s)} detached />
  }
}

function FloatingWindow({ w }: { w: DetailWindow }) {
  useObserve(w.session)
  const drag = (e: React.PointerEvent, resize: boolean) => {
    if (e.button !== 0) return
    e.preventDefault()
    const x0 = e.clientX, y0 = e.clientY
    const start = { x: w.x, y: w.y, width: w.width, height: w.height }
    const move = (m: PointerEvent) => {
      const dx = m.clientX - x0, dy = m.clientY - y0
      if (resize) app.moveWindow(w.id, { width: Math.max(360, start.width + dx), height: Math.max(240, start.height + dy) })
      else app.moveWindow(w.id, { x: Math.max(-start.width + 80, Math.min(window.innerWidth - 80, start.x + dx)), y: Math.max(0, Math.min(window.innerHeight - 40, start.y + dy)) })
    }
    const up = () => {
      window.removeEventListener('pointermove', move)
      window.removeEventListener('pointerup', up)
      document.body.classList.remove('dragging-window')
    }
    document.body.classList.add('dragging-window')
    window.addEventListener('pointermove', move)
    window.addEventListener('pointerup', up)
  }
  const front = app.frontWindow?.id === w.id
  return (
    <div className={`float-window${front ? ' front' : ''}`} style={{ left: w.x, top: w.y, width: w.width, height: w.height, zIndex: w.z }}
      data-window={w.id} onPointerDownCapture={() => app.focusWindow(w.id)} role="dialog" aria-label={title(w)}>
      <div className="float-title" onPointerDown={(e) => drag(e, false)} onDoubleClick={() => app.moveWindow(w.id, {
        x: 20, y: 20, width: window.innerWidth - 40, height: window.innerHeight - 40,
      })}>
        <div className="traffic">
          <button type="button" className="traffic-close" aria-label="閉じる" title="閉じる（⌘W の代わりに Esc）" onPointerDown={(e) => e.stopPropagation()} onClick={() => app.closeWindow(w.id)}>
            <Icon name="xmark" size={8} />
          </button>
        </div>
        <span className="float-title-text">{title(w)}</span>
        {w.session && <span className="float-subtitle">{w.session.scopeName([])}</span>}
      </div>
      <div className="float-body"><Content w={w} /></div>
      <div className="float-resize" onPointerDown={(e) => drag(e, true)} />
    </div>
  )
}

export function WindowHost() {
  useObserve(app)
  return <>{app.windows.map((w) => <FloatingWindow key={w.id} w={w} />)}</>
}
