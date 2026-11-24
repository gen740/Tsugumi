import { useCallback, useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { L, language } from '../i18n'
import { ViewMode, type LiveView } from '../model/liveViews'
import { useObserve } from '../model/observable'
import { errorText, type ProjectSession } from '../model/session'
import { DawnViewCanvas, type ReadDawnView } from './dawnViewCanvas'
import { HtmlViewFrame, type ReadHtmlView, type SendHtmlView } from './htmlViewFrame'

const windowLabels = {
  blocked: ['ポップアップがブロックされました。', 'The browser blocked the popup.'],
  open: ['別ウィンドウで表示', 'Show in a separate window'],
  panel: ['パネルで表示', 'Show in a panel'],
  fullscreen: ['フルスクリーンにする', 'Enter fullscreen'],
  unavailable: ['このブラウザーではフルスクリーンを利用できません。', 'Fullscreen is unavailable in this browser.'],
}
const label = (key: keyof typeof windowLabels) => windowLabels[key][language() === 'en' ? 1 : 0]

export function ProjectCardViews({ session }: { session: ProjectSession }) {
  useObserve(session)
  return <>{[...session.liveViews.values()].map((view, index) =>
    <CardViewWindow key={view.session} view={view} project={session} index={index} />)}</>
}

function CardViewWindow({ view, project, index }: { view: LiveView; project: ProjectSession; index: number }) {
  const [child, setChild] = useState<{ window: Window; document: Document }>()
  const [blocked, setBlocked] = useState(false)
  const [panel, setPanel] = useState(view.mode === ViewMode.panel)
  const title = `${project.displayName(view.card)} · ${view.name}`
  const close = useCallback(() => project.closeLiveView(view.session), [project, view])
  const open = useCallback(() => {
    const features = ['popup']
    if (view.width && view.height) features.push(`width=${view.width}`, `height=${view.height}`)
    const popup = window.open('', '_blank', features.join(','))
    if (!popup || popup.closed) {
      setBlocked(true)
      return
    }
    popup.document.documentElement.lang = language()
    const theme = document.documentElement.dataset.theme
    if (theme) popup.document.documentElement.dataset.theme = theme
    popup.document.body.className = 'card-view-child'
    for (const sheet of document.querySelectorAll<HTMLLinkElement>('link[rel="stylesheet"]')) {
      const link = popup.document.createElement('link')
      link.rel = 'stylesheet'
      link.href = sheet.href
      popup.document.head.append(link)
    }
    setChild({ window: popup, document: popup.document })
  }, [view])

  // One automatic attempt per session. A button can retry under user activation.
  useEffect(() => {
    if (view.mode !== ViewMode.panel) open()
  }, [open, view.mode])

  useEffect(() => {
    if (!child) return
    child.document.title = title
  }, [child, title])

  useEffect(() => {
    if (!child) return
    const popup = child.window
    popup.addEventListener('pagehide', close)
    window.addEventListener('pagehide', close)
    // Some browsers omit pagehide when closing a popup.
    const timer = window.setInterval(() => { if (popup.closed) close() }, 500)
    return () => {
      window.clearInterval(timer)
      window.removeEventListener('pagehide', close)
      if (!popup.closed) {
        popup.removeEventListener('pagehide', close)
        popup.close()
      }
    }
  }, [child, close])

  if (child) return createPortal(
    <ChildView fullscreen={view.mode === ViewMode.fullscreen}>
      <CardViewContent view={view} project={project} child />
    </ChildView>, child.document.body)
  if (!panel && !blocked) return null
  return <ViewPanel title={title} close={close} index={index}>
    {panel ? <CardViewContent view={view} project={project} /> :
      <div className="col gap8 pad16">
        <p role="status">{label('blocked')}</p>
        <div className="row gap8">
          <button type="button" onClick={open}>{label('open')}</button>
          <button type="button" onClick={() => setPanel(true)}>{label('panel')}</button>
        </div>
      </div>}
  </ViewPanel>
}

function ViewPanel({ title, close, index, children }: React.PropsWithChildren<{ title: string; close: () => void; index: number }>) {
  const offset = (index % 6) * 24
  const [position, setPosition] = useState(() => ({
    x: Math.max(12, (window.innerWidth - 760) / 2) + offset,
    y: Math.max(42, (window.innerHeight - 540) / 2) + offset,
  }))
  const drag = (event: React.PointerEvent<HTMLElement>) => {
    if (event.button !== 0) return
    const { clientX, clientY, pointerId, currentTarget } = event
    currentTarget.setPointerCapture(pointerId)
    const move = (e: PointerEvent) => {
      setPosition({
        x: Math.max(0, Math.min(window.innerWidth - 80, position.x + e.clientX - clientX)),
        y: Math.max(0, Math.min(window.innerHeight - 40, position.y + e.clientY - clientY)),
      })
    }
    currentTarget.addEventListener('pointermove', move)
    currentTarget.addEventListener('lostpointercapture', () => currentTarget.removeEventListener('pointermove', move), { once: true })
  }
  return <section className="float-window front card-view-panel" aria-label={title} role="dialog"
    style={{ left: position.x, top: position.y }}>
    <header className="float-title" onPointerDown={drag}>
      <button type="button" className="traffic-close" aria-label={L('閉じる')} title={L('閉じる')}
        onPointerDown={event => event.stopPropagation()} onClick={close}>×</button>
      <span className="float-title-text">{title}</span>
    </header>
    <div className="float-body">{children}</div>
  </section>
}

function ChildView({ fullscreen, children }: React.PropsWithChildren<{ fullscreen: boolean }>) {
  const surface = useRef<HTMLElement>(null)
  const [active, setActive] = useState(false)
  const [error, setError] = useState('')
  useEffect(() => {
    if (!fullscreen || !surface.current) return
    const target = surface.current
    const owner = target.ownerDocument
    const refresh = () => setActive(owner.fullscreenElement === target)
    owner.addEventListener('fullscreenchange', refresh)
    return () => owner.removeEventListener('fullscreenchange', refresh)
  }, [fullscreen])

  return <main className="card-view-child-content" ref={surface}>
    <div className="card-view-child-surface">{children}</div>
    {fullscreen && !active && <div className="card-view-fullscreen">
      <button type="button" onClick={() => {
        const target = surface.current!
        setError('')
        if (!target.requestFullscreen) {
          setError(label('unavailable'))
          return
        }
        void target.requestFullscreen().catch(cause => setError(errorText(cause)))
      }}>{label('fullscreen')}</button>
      {error && <p role="alert">{error}</p>}
    </div>}
  </main>
}

function CardViewContent({ view, project, child = false }: { view: LiveView; project: ProjectSession; child?: boolean }) {
  const client = project.rpc
  const read: ReadHtmlView = useCallback(async (target, signal) => {
    if (!client) throw new Error(L('接続していません'))
    return await client.readCardView(target.session, signal)
  }, [client])
  const readBytes: ReadDawnView = useCallback(async (target, signal) => {
    if (!client) throw new Error(L('接続していません'))
    return await client.readCardViewBytes(target.session, signal)
  }, [client])
  const send: SendHtmlView = useCallback(async (target, bytes, route) => {
    if (!client) throw new Error(L('接続していません'))
    return await client.sendView(target.session, route, bytes)
  }, [client])
  return <CardViewSurface view={view} read={read} readBytes={readBytes} send={send} child={child} />
}

export function CardViewSurface({ view, read, readBytes, send, child = false }: {
  view: LiveView; read: ReadHtmlView; readBytes: ReadDawnView; send: SendHtmlView; child?: boolean
}) {
  useEffect(() => {
    if (view.type !== 'html' && view.type !== 'dawn')
      return view.listen(message => message.complete?.())
  }, [view])
  return <section className="card-view">
    {view.type === 'html' ? <HtmlViewFrame view={view} read={read} send={send} child={child} />
      : view.type === 'dawn' ? <DawnViewCanvas view={view} read={readBytes} send={send} />
      : <p className="caption" role="alert">{L('未対応のビュー形式: {0}', view.type)}</p>}
  </section>
}
