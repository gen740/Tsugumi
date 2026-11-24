import { useEffect, useRef, useState } from 'react'
import { L } from '../i18n'
import type { LiveView, ViewMessage } from '../model/liveViews'
import { errorText } from '../model/session'

export type SendHtmlView = (view: LiveView, bytes: Uint8Array<ArrayBuffer>, route: Uint8Array<ArrayBuffer>) => Promise<unknown>
export type ReadHtmlView = (view: LiveView, signal: AbortSignal) => Promise<string>

/** Each mount owns both frame contexts and the subscription to its view. */
export function HtmlViewFrame({ view, read, send, child }: { view: LiveView; read: ReadHtmlView; send: SendHtmlView; child: boolean }) {
  const frame = useRef<HTMLIFrameElement>(null)
  const url = child ? new URL('./modules/viewer.html?child=1', document.baseURI).href : './modules/viewer.html'
  const [error, setError] = useState('')
  const [messageError, setMessageError] = useState('')
  useEffect(() => {
    const target = frame.current
    const owner = target?.ownerDocument.defaultView
    if (!target || !owner) return
    const loading = new AbortController()
    let html: string | undefined
    let ready = false
    let mounted = false
    let pending: ViewMessage[] = []
    let scheduled = false
    let ticket = 0
    const outstanding = new Map<number, () => void>()
    const messageMax = 1 << 24
    let reverseBytes = 0
    const reverse: { bytes: Uint8Array<ArrayBuffer>; route: Uint8Array<ArrayBuffer> }[] = []
    let sending = false
    const release = () => {
      for (const message of pending) message.complete?.()
      for (const complete of outstanding.values()) complete()
      pending = []
      outstanding.clear()
    }
    const post = (message: object, transfer: Transferable[] = []) =>
      target?.contentWindow?.postMessage({ tsugumi: 'view', message }, '*', transfer)
    const flush = () => {
      scheduled = false
      if (loading.signal.aborted || !ready || html === undefined) return
      if (!mounted) {
        post({ type: 'mount', html })
        mounted = true
      }
      for (const message of pending) {
        const bytes = message.bytes.slice().buffer
        const id = ++ticket
        if (message.complete) outstanding.set(id, message.complete)
        post({ type: 'data', bytes, ticket: id }, [bytes])
      }
      pending = []
    }
    const schedule = () => {
      if (scheduled) return
      scheduled = true
      queueMicrotask(flush)
    }
    const stop = view.listen(message => {
      pending.push(message)
      schedule()
    })
    const flushReverse = async () => {
      if (sending || loading.signal.aborted) return
      sending = true
      while (reverse.length && !loading.signal.aborted) {
        const { bytes, route } = reverse.shift()!
        try {
          await send(view, bytes, route)
        } catch (cause) {
          if (!loading.signal.aborted) setMessageError(errorText(cause))
        } finally {
          reverseBytes -= Math.max(1, bytes.length)
          if (!loading.signal.aborted) post({ type: 'reverseCredit', size: bytes.length })
        }
      }
      sending = false
    }
    const receive = (event: MessageEvent) => {
      if (event.source !== target?.contentWindow) return
      const data = event.data
      if (data?.tsugumi === 'ready') {
        ready = true
        schedule()
      } else if (data?.tsugumi === 'credit') {
        outstanding.get(data.ticket)?.()
        outstanding.delete(data.ticket)
      } else if (data?.tsugumi === 'messageDropped') {
        setMessageError(L('ビューのメッセージを送信できませんでした'))
      } else if (data?.tsugumi === 'send' && data.bytes instanceof owner.ArrayBuffer &&
        data.route instanceof owner.ArrayBuffer && data.route.byteLength === 16) {
        const bytes = new Uint8Array(data.bytes)
        if (bytes.length > messageMax || reverseBytes > messageMax) {
          setMessageError(L('ビューのメッセージを送信できませんでした'))
          post({ type: 'reverseCredit', size: bytes.length })
        } else {
          reverseBytes += Math.max(1, bytes.length)
          reverse.push({ bytes, route: new Uint8Array(data.route) })
          void flushReverse()
        }
      }
    }
    owner.addEventListener('message', receive)
    target?.contentWindow?.postMessage({ tsugumi: 'probe' }, '*')
    setError('')
    setMessageError('')
    void read(view, loading.signal).then(source => {
      if (loading.signal.aborted) return
      html = source
      schedule()
    }).catch(cause => {
      if (loading.signal.aborted) return
      release()
      stop()
      setError(errorText(cause))
    })
    return () => {
      loading.abort()
      release()
      stop()
      owner.removeEventListener('message', receive)
      post({ type: 'dispose' })
    }
  }, [view, read, send, url])

  return <>
    <iframe ref={frame} sandbox="allow-scripts" src={url} title={view.name} hidden={!!error} />
    {messageError && <p className="caption" role="alert">{messageError}</p>}
    {error && <p className="caption" role="alert">{L('ビューを読み込めません: {0}', error)}</p>}
  </>
}
