import { useEffect, useRef, useState } from 'react'
import { L } from '../i18n'
import type { CardLiveViews, LiveView, ViewMessage } from '../model/liveViews'
import { errorText } from '../model/session'

export type ReadHtmlView = (view: LiveView, signal: AbortSignal) => Promise<string>

/** Each mount owns both frame contexts and the subscription to its view. */
export function HtmlViewFrame({ view, card, read }: { view: LiveView; card: CardLiveViews; read: ReadHtmlView }) {
  const frame = useRef<HTMLIFrameElement>(null)
  const [error, setError] = useState('')
  useEffect(() => {
    const target = frame.current
    const loading = new AbortController()
    let html: string | undefined
    let ready = false
    let mounted = false
    let pending: ViewMessage[] = []
    let scheduled = false
    const post = (message: object, transfer: Transferable[] = []) =>
      target?.contentWindow?.postMessage({ tsugumi: 'view', message }, '*', transfer)
    const flush = () => {
      scheduled = false
      if (loading.signal.aborted || card.views[view.index] !== view || !ready || html === undefined) return
      if (!mounted) {
        post({ type: 'mount', html })
        mounted = true
      }
      for (const message of pending) {
        if (message.type === 'data') {
          const bytes = message.bytes.slice().buffer
          post({ type: 'data', bytes }, [bytes])
        } else post(message)
      }
      pending = []
    }
    const schedule = () => {
      if (scheduled) return
      scheduled = true
      queueMicrotask(flush)
    }
    const stop = view.listen(card.activeRun, message => {
      if (message.type === 'data' && pending.at(-1)?.type === 'data') pending[pending.length - 1] = message
      else pending.push(message)
      schedule()
    })
    const receive = (event: MessageEvent) => {
      if (event.source !== target?.contentWindow || event.data?.tsugumi !== 'ready') return
      ready = true
      schedule()
    }
    window.addEventListener('message', receive)
    target?.contentWindow?.postMessage({ tsugumi: 'probe' }, '*')
    setError('')
    void read(view, loading.signal).then(source => {
      if (loading.signal.aborted) return
      html = source
      schedule()
    }).catch(cause => {
      if (loading.signal.aborted) return
      pending = []
      stop()
      setError(errorText(cause))
    })
    return () => {
      loading.abort()
      stop()
      window.removeEventListener('message', receive)
      post({ type: 'dispose' })
    }
  }, [view, card, read])

  return <>
    <iframe ref={frame} sandbox="allow-scripts" src="./modules/viewer.html" title={view.declaration.name} hidden={!!error} />
    {error && <p className="caption" role="alert">{L('ビューを読み込めません: {0}', error)}</p>}
  </>
}
