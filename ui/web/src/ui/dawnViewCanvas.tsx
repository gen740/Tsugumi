import { useEffect, useRef, useState } from 'react'
import { browserGpu, DawnViewInstance } from '../dawn/instance'
import { loadView } from '../dawn/loadView'
import { L } from '../i18n'
import type { LiveView, ViewMessage } from '../model/liveViews'
import { errorText } from '../model/session'
import type { SendHtmlView } from './htmlViewFrame'

export type ReadDawnView = (view: LiveView, signal: AbortSignal) => Promise<Uint8Array<ArrayBuffer>>

// TODO(open): Agent PNG snapshots of dawn views.
export function DawnViewCanvas({ view, read, send }: { view: LiveView; read: ReadDawnView; send: SendHtmlView }) {
  const canvas = useRef<HTMLCanvasElement>(null)
  const [error, setError] = useState('')
  const [gpuError, setGpuError] = useState('')
  const [sendError, setSendError] = useState('')
  const supported = !!browserGpu()

  useEffect(() => {
    const target = canvas.current
    if (!target) return view.listen(message => message.complete?.())
    const loading = new AbortController()
    const { signal } = loading
    let instance: DawnViewInstance | undefined
    const pending: ViewMessage[] = []
    const stop = view.listen(message => {
      if (instance) instance.deliver(message)
      else pending.push(message)
    })
    const releasePending = () => {
      for (const message of pending.splice(0)) message.complete?.()
    }

    setError('')
    setGpuError('')
    setSendError('')
    void (async () => {
      const { module } = await loadView(await read(view, signal))
      signal.throwIfAborted()
      const ready = await DawnViewInstance.mount(module, target, {
        send: (bytes, route) => send(view, bytes, route),
        acceptsRoute: route => view.acceptsRoute(route),
        onFatal: cause => { if (!signal.aborted) setError(errorText(cause)) },
        onGpuError: message => { if (!signal.aborted) setGpuError(message) },
        onSendError: cause => {
          if (!signal.aborted) setSendError(cause === undefined ? L('ビューのメッセージを送信できませんでした') : errorText(cause))
        },
      }, signal)
      if (signal.aborted) {
        ready.dispose()
        return
      }
      instance = ready
      for (const message of pending.splice(0)) instance.deliver(message)
    })().catch(cause => {
      if (signal.aborted) return
      stop()
      releasePending()
      setError(errorText(cause))
    })

    return () => {
      loading.abort()
      stop()
      releasePending()
      instance?.dispose()
    }
  }, [view, read, send, supported])

  if (!supported)
    return <p className="caption" role="alert">{L('このブラウザーでは WebGPU を利用できません。')}</p>

  return <>
    <div className="dawn-view-surface">
      <canvas ref={canvas} tabIndex={0} aria-label={view.name} />
    </div>
    {error && <p className="caption" role="alert">{L('Dawn ビューのエラー: {0}', error)}</p>}
    {gpuError && <p className="caption" role="alert">{L('WebGPU エラー: {0}', gpuError)}</p>}
    {sendError && <p className="caption" role="alert">{sendError}</p>}
  </>
}
