import { useEffect, useRef, useState } from 'react'
import * as H from '../../shared/gen/tsugumi/host'
import { L, language } from '../i18n'
import { errorText } from '../model/session'
import { hostCall } from '../wire/host'
import { s } from '../wire/rpc'
import { Button, Spinner } from './controls'

const operations = new Map<string, H.WorkerDashboardOperation>([
  ['Jobs', H.WorkerDashboardOperation.Jobs],
  ['Keys', H.WorkerDashboardOperation.Keys],
  ['Challenge', H.WorkerDashboardOperation.Challenge],
  ['RemoveJobs', H.WorkerDashboardOperation.RemoveJobs],
  ['AddKey', H.WorkerDashboardOperation.AddKey],
  ['RemoveKey', H.WorkerDashboardOperation.RemoveKey],
])

const relay = (request: H.WorkerDashboardRequestT) =>
  hostCall<H.WorkerDashboardReplyT>(H.RequestPayload.WorkerDashboardRequest,
    request, H.ReplyPayload.WorkerDashboardReply)

export function WorkerDashboard({ id, name, close }: { id: string; name: string; close: () => void }) {
  const [page, setPage] = useState<H.WorkerDashboardReplyT>()
  const [error, setError] = useState('')
  useEffect(() => {
    let closed = false
    void relay(new H.WorkerDashboardRequestT(id, H.WorkerDashboardOperation.Page, language())).then(
      result => { if (!closed) setPage(result) },
      cause => { if (!closed) setError(errorText(cause)) },
    )
    return () => { closed = true }
  }, [id])

  return <div className="worker-dashboard">
    <div className="worker-dashboard-toolbar">
      <Button onClick={close}>{L('戻る')}</Button>
      <strong>{name}</strong>
    </div>
    {page ? <DashboardFrame id={id} page={page} /> :
      error ? <p role="alert">{L('ワーカーのダッシュボードを読み込めません: {0}', error)}</p> :
        <p role="status"><Spinner small /> {L('ダッシュボードを読み込み中…')}</p>}
  </div>
}

function DashboardFrame({ id, page }: { id: string; page: H.WorkerDashboardReplyT }) {
  const bridge = useRef<MessagePort>()
  useEffect(() => () => { bridge.current?.close(); bridge.current = undefined }, [])
  const title = L('ワーカー管理')
  return <iframe
    className="worker-dashboard-frame" title={title} sandbox="allow-scripts"
    src="./modules/worker-dashboard.html" referrerPolicy="no-referrer"
    onLoad={event => {
      const target = event.currentTarget.contentWindow
      if (!target || bridge.current) return
      const { port1, port2 } = new MessageChannel()
      bridge.current = port1
      let pending = 0
      port1.onmessage = ({ data }) => {
        if (!Number.isSafeInteger(data?.id)) return
        const operation = operations.get(data.operation)
        if (operation === undefined || typeof data.body !== 'string' || data.body.length > 65536 || pending >= 4) {
          port1.postMessage({ id: data.id, error: 'Invalid dashboard request' })
          return
        }
        const ticket = data.id
        pending++
        void relay(new H.WorkerDashboardRequestT(id, operation, null, s(page.generation), data.body)).then(
          result => port1.postMessage({ id: ticket, body: s(result.body) }),
          cause => port1.postMessage({ id: ticket, error: errorText(cause) }),
        ).finally(() => { pending-- })
      }
      target.postMessage({ type: 'worker-dashboard', html: s(page.body), title }, '*', [port2])
    }}
  />
}
