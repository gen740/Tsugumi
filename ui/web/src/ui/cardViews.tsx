// Card views in in-page windows. Every view of a project stays in one place in
// this document: docking it as a panel, detaching it into a window, filling
// the page or minimizing it only changes its position, size and visibility, so
// an html view's frame and a dawn view's Worker keep running untouched.
import { useCallback, useEffect, useRef, useState } from 'react'
import { L } from '../i18n'
import { ViewMode, type LiveView } from '../model/liveViews'
import { Observable, useObserve } from '../model/observable'
import { errorText, type ProjectSession } from '../model/session'
import { raiseWindow, topWindow } from '../model/windowOrder'
import { DawnViewCanvas, type ReadDawnView } from './dawnViewCanvas'
import { HtmlViewFrame, type ReadHtmlView, type SendHtmlView } from './htmlViewFrame'
import { Icon } from './icons'

/** Where a view window is: docked over the canvas, floating, or filling the page. */
export type ViewPlacement = 'panel' | 'window' | 'page'
type Rect = { x: number; y: number; width: number; height: number }

const titleHeight = 30
const defaultSize = { width: 760, height: 510 }

export class ViewFrame {
  minimized = false
  z = raiseWindow()
  /** The floating rectangle, kept while the view is docked or fills the page. */
  rect: Rect

  constructor(public placement: ViewPlacement, view: LiveView, index: number) {
    const width = Math.min(view.width || defaultSize.width, window.innerWidth - 24)
    const height = Math.min((view.height || defaultSize.height) + titleHeight, window.innerHeight - 24)
    const offset = (index % 6) * 24
    this.rect = {
      x: Math.max(12, (window.innerWidth - width) / 2 + offset),
      y: Math.max(12, (window.innerHeight - height) / 2 + offset),
      width, height,
    }
  }
}

/** The presentation of every open view; the views themselves live in their project. */
class ViewFrames extends Observable {
  private readonly frames = new WeakMap<LiveView, ViewFrame>()

  frame(view: LiveView, index: number): ViewFrame {
    let frame = this.frames.get(view)
    if (!frame) {
      const placement = view.mode === ViewMode.window ? 'window' : view.mode === ViewMode.fullscreen ? 'page' : 'panel'
      frame = new ViewFrame(placement, view, index)
      this.frames.set(view, frame)
    }
    return frame
  }

  update(frame: ViewFrame, change: Partial<Pick<ViewFrame, 'placement' | 'minimized' | 'rect'>>) {
    Object.assign(frame, change)
    if (change.minimized === false || change.placement) frame.z = raiseWindow()
    this.changed()
  }

  focus(frame: ViewFrame) {
    if (topWindow() === frame.z) return
    frame.z = raiseWindow()
    this.changed()
  }
}

export const viewFrames = new ViewFrames()

/** The canvas area that panels dock to; the page when there is none. */
function useDockArea(): Rect {
  const measure = () => {
    const area = document.querySelector('.workbench-canvas')?.getBoundingClientRect()
    return area ? { x: area.left, y: area.top, width: area.width, height: area.height }
      : { x: 0, y: 0, width: window.innerWidth, height: window.innerHeight }
  }
  const [area, setArea] = useState(measure)
  useEffect(() => {
    const refresh = () => setArea((previous: Rect) => {
      const next = measure()
      return Object.entries(next).every(([key, value]) => previous[key as keyof Rect] === value) ? previous : next
    })
    const observer = new ResizeObserver(refresh)
    let canvas: Element | null | undefined
    const watch = () => {
      const current = document.querySelector('.workbench-canvas')
      if (current === canvas) return
      canvas = current
      observer.disconnect()
      observer.observe(document.body)
      if (canvas) observer.observe(canvas)
      refresh()
    }
    // The canvas area comes and goes with the project page.
    const mutations = new MutationObserver(watch)
    mutations.observe(document.body, { childList: true, subtree: true })
    window.addEventListener('resize', refresh)
    watch()
    return () => {
      observer.disconnect()
      mutations.disconnect()
      window.removeEventListener('resize', refresh)
    }
  }, [])
  return area
}

/** Panels share the right side of the canvas, one above another. */
function panelRect(area: Rect, index: number, count: number): Rect {
  const inset = 8
  const width = Math.min(area.width - 2 * inset, Math.max(320, Math.round(area.width * 0.42)))
  const height = (area.height - inset * (count + 1)) / count
  return { x: area.x + area.width - width - inset, y: area.y + inset + index * (height + inset), width, height }
}

export function ProjectCardViews({ session }: { session: ProjectSession }) {
  useObserve(session, viewFrames)
  const area = useDockArea()
  const views = [...session.liveViews.values()]
  const frames = views.map((view, index) => viewFrames.frame(view, index))
  const panels = views.filter((_, index) => frames[index].placement === 'panel' && !frames[index].minimized)

  // Focus inside an html view's frame never reaches this document as a click.
  useEffect(() => {
    const blur = () => setTimeout(() => {
      const active = document.activeElement
      const session = active instanceof HTMLIFrameElement ? active.closest<HTMLElement>('[data-view-window]')?.dataset.viewWindow : undefined
      const index = views.findIndex(view => view.session === session)
      if (index >= 0) viewFrames.focus(frames[index])
    })
    window.addEventListener('blur', blur)
    return () => window.removeEventListener('blur', blur)
  })

  const minimized = views.filter((_, index) => frames[index].minimized)
  return <>
    {views.map((view, index) => {
      const frame = frames[index]
      const panel = panels.indexOf(view)
      return <CardViewWindow key={view.session} view={view} frame={frame} project={session}
        dock={panel >= 0 ? panelRect(area, panel, panels.length) : undefined} />
    })}
    {minimized.length > 0 && <nav className="view-dock" aria-label={L('最小化したビュー')}>
      {minimized.map(view => <button key={view.session} type="button" className="view-dock-item"
        title={L('元に戻す')} onClick={() => viewFrames.update(viewFrames.frame(view, 0), { minimized: false })}>
        {viewTitle(session, view)}
      </button>)}
    </nav>}
  </>
}

const viewTitle = (project: ProjectSession, view: LiveView) => `${project.displayName(view.card)} · ${view.name}`

function CardViewWindow({ view, frame, project, dock }: { view: LiveView; frame: ViewFrame; project: ProjectSession; dock?: Rect }) {
  const element = useRef<HTMLElement>(null)
  const [fullscreen, setFullscreen] = useState(false)
  const [error, setError] = useState('')
  const title = viewTitle(project, view)
  const close = useCallback(() => project.closeLiveView(view.session), [project, view])

  useEffect(() => {
    const refresh = () => setFullscreen(!!element.current && document.fullscreenElement === element.current)
    document.addEventListener('fullscreenchange', refresh)
    return () => document.removeEventListener('fullscreenchange', refresh)
  }, [])

  const place = (placement: ViewPlacement) => {
    if (fullscreen) void document.exitFullscreen().catch(() => {})
    viewFrames.update(frame, { placement })
  }
  const minimize = () => {
    if (fullscreen) void document.exitFullscreen().catch(() => {})
    if (element.current?.contains(document.activeElement)) (document.activeElement as HTMLElement).blur?.()
    viewFrames.update(frame, { minimized: true })
  }
  const enterFullscreen = () => {
    setError('')
    const target = element.current
    if (!target?.requestFullscreen) {
      setError(L('このブラウザーではフルスクリーンを利用できません。'))
      return
    }
    void target.requestFullscreen().catch(cause => setError(errorText(cause)))
  }

  const drag = (event: React.PointerEvent, resize: boolean) => {
    if (event.button !== 0 || frame.placement !== 'window') return
    event.preventDefault()
    const x0 = event.clientX, y0 = event.clientY
    const start = frame.rect
    const move = (m: PointerEvent) => {
      const dx = m.clientX - x0, dy = m.clientY - y0
      viewFrames.update(frame, { rect: resize
        ? { ...start, width: Math.max(240, start.width + dx), height: Math.max(160, start.height + dy) }
        : {
          ...start,
          x: Math.max(80 - start.width, Math.min(window.innerWidth - 80, start.x + dx)),
          y: Math.max(0, Math.min(window.innerHeight - titleHeight, start.y + dy)),
        } })
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

  const rect = frame.placement === 'window' ? frame.rect : frame.placement === 'panel' ? dock ?? frame.rect : undefined
  const front = topWindow() === frame.z
  const button = (label: string, icon: string, action: () => void) =>
    <button type="button" className="view-window-button" aria-label={label} title={label}
      onPointerDown={event => event.stopPropagation()} onClick={action}><Icon name={icon} size={12} /></button>

  return <section ref={element} data-view-window={view.session} role="dialog" aria-label={title}
    aria-hidden={frame.minimized || undefined}
    className={`float-window view-window view-window-${frame.placement}${front ? ' front' : ''}${frame.minimized ? ' minimized' : ''}`}
    style={{ zIndex: frame.z, ...(rect && { left: rect.x, top: rect.y, width: rect.width, height: rect.height }) }}
    onPointerDownCapture={() => viewFrames.focus(frame)} onFocusCapture={() => viewFrames.focus(frame)}>
    <header className="float-title" onPointerDown={event => drag(event, false)}
      onDoubleClick={() => frame.placement !== 'panel' && place(frame.placement === 'page' ? 'window' : 'page')}>
      <div className="traffic">
        <button type="button" className="traffic-close" aria-label={L('閉じる')} title={L('閉じる')}
          onPointerDown={event => event.stopPropagation()} onClick={close}><Icon name="xmark" size={8} /></button>
        <button type="button" className="traffic-minimize" aria-label={L('最小化')} title={L('最小化')}
          onPointerDown={event => event.stopPropagation()} onClick={minimize}><Icon name="minus" size={8} /></button>
      </div>
      <span className="float-title-text">{title}</span>
      <div className="view-window-buttons">
        {frame.placement === 'panel' && button(L('ウィンドウで表示'), 'square.on.square', () => place('window'))}
        {frame.placement === 'window' && button(L('パネルで表示'), 'sidebar.right', () => place('panel'))}
        {frame.placement === 'window' && button(L('ページ全体に表示'), 'arrow.up.left.and.arrow.down.right', () => place('page'))}
        {frame.placement === 'page' && button(L('ウィンドウで表示'), 'arrow.down.right.and.arrow.up.left', () => place('window'))}
        {frame.placement === 'page' && !fullscreen && button(L('フルスクリーンにする'), 'viewfinder', enterFullscreen)}
      </div>
    </header>
    {error && <p className="caption view-window-error" role="alert">{error}</p>}
    <div className="float-body"><CardViewContent view={view} project={project} hidden={frame.minimized} /></div>
    {frame.placement === 'window' && <div className="float-resize" onPointerDown={event => drag(event, true)} />}
  </section>
}

function CardViewContent({ view, project, hidden }: { view: LiveView; project: ProjectSession; hidden: boolean }) {
  const client = project.rpc
  const read: ReadHtmlView = useCallback(async (target, signal) => {
    if (!client) throw new Error(L('接続していません'))
    return await client.readCardView(target.session, signal)
  }, [client])
  const readBytes: ReadDawnView = useCallback(async (target, signal) => {
    if (!client) throw new Error(L('接続していません'))
    return await client.readCardViewBytes(target.session, signal)
  }, [client])
  const send: SendHtmlView = useCallback(async (target, bytes, to) => {
    if (!client) throw new Error(L('接続していません'))
    return await client.sendView(target.session, to, bytes)
  }, [client])
  return <CardViewSurface view={view} read={read} readBytes={readBytes} send={send} hidden={hidden} />
}

export function CardViewSurface({ view, read, readBytes, send, hidden = false }: {
  view: LiveView; read: ReadHtmlView; readBytes: ReadDawnView; send: SendHtmlView; hidden?: boolean
}) {
  useEffect(() => {
    if (view.type !== 'html' && view.type !== 'dawn')
      return view.listen(message => message.complete?.())
  }, [view])
  return <section className="card-view">
    {view.type === 'html' ? <HtmlViewFrame view={view} read={read} send={send} />
      : view.type === 'dawn' ? <DawnViewCanvas view={view} read={readBytes} send={send} hidden={hidden} />
      : <p className="caption" role="alert">{L('未対応のビュー形式: {0}', view.type)}</p>}
  </section>
}
