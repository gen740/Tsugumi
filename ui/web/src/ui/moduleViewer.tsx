import { useEffect, useMemo, useRef, useState } from 'react'
import { L } from '../i18n'
import { moduleSource } from '../model/moduleSources'
import { useObserve } from '../model/observable'
import { prefs } from '../model/prefs'
import { displayModule, DisplayModule, DisplaySnapshot } from '../model/session'
import { Empty } from './controls'

export function DisplayView({ display, emptyNote }: { display?: DisplaySnapshot; emptyNote?: string }) {
  const module = useMemo(() => display && displayModule(display.value), [display])
  if (!display) return <Empty title={L('まだ結果がありません')} icon="chart.xyaxis.line" description={emptyNote ?? L('実行すると表示されます。')} />
  if (!module) return <Empty title={L('この結果は表示できません')} icon="chart.xyaxis.line" />
  return <ModuleViewer key={module.name} module={module} />
}

/** An identity change unmounts the iframe and disposes of its entire context. */
function ModuleViewer({ module }: { module: DisplayModule }) {
  useObserve(prefs)
  const theme = document.documentElement.dataset.theme === 'dark' ? 'dark' : 'light'
  const frame = useRef<HTMLIFrameElement>(null)
  const sent = useRef<DisplayModule>()
  const [ready, setReady] = useState(false)
  const [error, setError] = useState('')
  useEffect(() => {
    if (!frame.current) return
    const receive = (event: MessageEvent) => {
      if (event.source !== frame.current?.contentWindow) return
      if (event.data?.tsugumi === 'ready') setReady(true)
      if (event.data?.tsugumi === 'state') setError(event.data.state === 'error' ? String(event.data.message).slice(0, 4096) : '')
      if (event.data?.tsugumi === 'key' && event.data.key === 'Escape') document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
    }
    window.addEventListener('message', receive)
    frame.current.contentWindow?.postMessage({ tsugumi: 'probe' }, '*')
    return () => window.removeEventListener('message', receive)
  }, [])
  useEffect(() => {
    if (!ready) return
    let cancelled = false
    const send = (code?: string) => {
      if (cancelled) return
      // The sandbox gives the frame an opaque origin.
      frame.current?.contentWindow?.postMessage({
        name: module.name, colorScheme: theme, code,
        ...(sent.current !== module ? { config: module.config, blobs: module.blobs } : {}),
      }, '*')
      sent.current = module
    }
    if (sent.current) send()
    else void moduleSource(module.name).then(send).catch(error => { if (!cancelled) setError(String(error)) })
    return () => { cancelled = true }
  }, [ready, module, theme])
  return <div className="display-viewer">
    <iframe ref={frame} sandbox="allow-scripts" src="/modules/viewer.html" title={L('可視化')} />
    {error && <div className="secondary caption" role="alert">{error}</div>}
  </div>
}
