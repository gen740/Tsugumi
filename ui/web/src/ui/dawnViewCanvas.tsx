import { useEffect, useRef, useState } from 'react'
import { BrowserView } from '../dawn/browserView'
import { L } from '../i18n'
import type { LiveView } from '../model/liveViews'
import { errorText } from '../model/session'
import type { SendHtmlView } from './htmlViewFrame'

export type ReadDawnView = (view: LiveView, signal: AbortSignal) => Promise<Uint8Array<ArrayBuffer>>

export function DawnViewCanvas({ view, read, send }: { view: LiveView; read: ReadDawnView; send: SendHtmlView }) {
  const surface = useRef<HTMLDivElement>(null)
  const controller = useRef<BrowserView>()
  const [error, setError] = useState('')
  const [gpuError, setGpuError] = useState('')
  const [sendError, setSendError] = useState('')
  const [audioError, setAudioError] = useState('')
  const [enableAudio, setEnableAudio] = useState(false)
  const [waiting, setWaiting] = useState(false)
  const supported = typeof Worker !== 'undefined' && typeof HTMLCanvasElement !== 'undefined'
    && typeof HTMLCanvasElement.prototype.transferControlToOffscreen === 'function' && 'gpu' in navigator

  useEffect(() => {
    const target = surface.current
    if (!target || !supported) return view.listen(message => message.complete?.())
    const loading = new AbortController()
    const { signal } = loading
    setError('')
    setGpuError('')
    setSendError('')
    setAudioError('')
    setWaiting(false)

    // A canvas can be transferred just once. Create the presentation elements
    // for this renderer's lifecycle, including a fresh canvas on remount.
    const canvas = target.ownerDocument.createElement('canvas')
    canvas.tabIndex = 0
    canvas.setAttribute('aria-label', view.name)
    const textarea = target.ownerDocument.createElement('textarea')
    textarea.className = 'dawn-view-text'
    textarea.hidden = true
    textarea.tabIndex = -1
    textarea.setAttribute('aria-label', view.name)
    target.replaceChildren(canvas, textarea)

    let instance: BrowserView
    try {
      instance = new BrowserView(canvas, textarea, view.flags, {
        send: (bytes, route) => send(view, bytes, route),
        acceptsRoute: route => view.acceptsRoute(route),
        fatal: setError,
        gpuError: setGpuError,
        sendError: cause => setSendError(cause || L('ビューのメッセージを送信できませんでした')),
        audioError: setAudioError,
        waiting: setWaiting,
        enableAudio: setEnableAudio,
      })
    } catch (cause) {
      setError(errorText(cause))
      return view.listen(message => message.complete?.())
    }
    controller.current = instance
    const detach = view.attachRenderer(instance)
    const unlisten = view.listen(message => instance.deliver(message))
    void (async () => {
      const bytes = await read(view, signal)
      if (!signal.aborted) instance.start(bytes)
    })().catch(cause => {
      if (!signal.aborted) {
        setError(errorText(cause))
        instance.dispose()
      }
    })

    return () => {
      loading.abort()
      unlisten()
      detach()
      target.replaceChildren()
      controller.current = undefined
    }
  }, [view, read, send, supported])

  if (!supported)
    return <p className="caption" role="alert">{L('このブラウザーでは WebGPU を利用できません。')}</p>

  return <>
    <div className="dawn-view-surface">
      <div ref={surface} />
      {enableAudio && !waiting && <button className="dawn-view-audio" type="button" onClick={() => controller.current?.enableAudio()}>
        {L('音声を有効にする')}
      </button>}
      {waiting && <div className="dawn-view-waiting">
        <p role="status">{L('ビューの応答を待っています')}</p>
        <button type="button" onClick={() => {
          controller.current?.dispose()
          setError(L('ビューを停止しました'))
        }}>{L('停止')}</button>
      </div>}
    </div>
    {error && <p className="caption" role="alert">{L('Dawn ビューのエラー: {0}', error)}</p>}
    {gpuError && <p className="caption" role="alert">{L('WebGPU エラー: {0}', gpuError)}</p>}
    {audioError && <p className="caption" role="alert">{L('音声のエラー: {0}', audioError)}</p>}
    {sendError && <p className="caption" role="alert">{sendError}</p>}
  </>
}
