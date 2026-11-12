import { useEffect, useRef, useState } from 'react'
import { L } from '../i18n'
import { CardKey, cardState, errorText, path, ProjectSession } from '../model/session'
import { R, sOpt } from '../shared/rpc'
import { Button, Popover, Spinner } from './controls'

/** Refresh the open history views after member updates, at a bounded rate. */
export function useBatchHistoryRevision(session: ProjectSession, enabled = true): number {
  const [revision, setRevision] = useState(session.batchHistoryRevision)
  useEffect(() => {
    if (!enabled) return
    const update = () => setRevision(session.batchHistoryRevision)
    update()
    const timer = setInterval(update, 200)
    return () => clearInterval(timer)
  }, [session, enabled])
  return revision
}

export function BatchInvocations({ session, card, batch, onLog, onHistory }: {
  session: ProjectSession
  card: CardKey
  batch: R.RunBatchT
  onLog: (invocation: bigint) => void
  onHistory: () => void
}) {
  const anchor = useRef<HTMLButtonElement>(null)
  const [open, setOpen] = useState(false)
  const [page, setPage] = useState<R.InvocationPageT>()
  const [cursors, setCursors] = useState<string[]>([])
  const [problem, setProblem] = useState<string>()
  const runId = session.currentRunId
  const rpc = session.rpc
  const cursor = cursors.at(-1)
  const revision = useBatchHistoryRevision(session, open)
  useEffect(() => {
    setPage(undefined)
    setCursors([])
  }, [runId, batch.activation])
  useEffect(() => {
    if (!open || !rpc || !runId) return
    let active = true
    void rpc.invocations(runId, path(card), cursor ?? null).then((result) => {
      if (!active) return
      setPage(result)
      setProblem(undefined)
    }).catch((error) => { if (active) setProblem(errorText(error)) })
    return () => { active = false }
  }, [open, rpc, runId, card, cursor, revision])

  const next = sOpt(page?.nextCursor)
  return <div className="cv-batch" onPointerDown={(event) => event.stopPropagation()} onDoubleClick={(event) => event.stopPropagation()}>
    <button type="button" ref={anchor} className="cv-batch-count mono" aria-expanded={open}
      aria-label={L('呼び出し一覧（{0} 件）', batch.invocationCount)} onClick={() => setOpen(!open)}>
      ×{batch.invocationCount}
    </button>
    <Popover open={open} onClose={() => setOpen(false)} anchor={anchor} width={350}>
      <div className="cv-batch-panel col gap6">
        <div className="row gap6 center">
          <strong>{L('呼び出し一覧')}</strong><span className="spacer" />
          <Button small onClick={() => { setOpen(false); onHistory() }}>{L('実行履歴')}</Button>
        </div>
        {problem ? <div className="caption selectable">{problem}</div> : !rpc ? <div className="caption">{L('接続していません')}</div>
          : !page ? <Spinner small /> : !page.records.length ? <span className="caption secondary">{batch.invocationCount === 0
            ? L('空の実行リスト') : L('呼び出しの記録を待っています')}</span> : (
            <div className="cv-batch-rows">
              {page.records.map((item) => item.invocation != null && <button className="cv-batch-row" type="button"
                key={String(item.invocation)} onClick={() => { if (runId === session.currentRunId) { setOpen(false); onLog(item.invocation!) } }}>
                <span className="mono">#{String(item.invocation)}</span>
                {item.batchIndex != null && item.batchSize != null && <span className="caption secondary">
                  {item.batchActivation != null && `#${item.batchActivation} · `}{item.batchIndex}/{item.batchSize}
                </span>}
                <span className="spacer" /><span>{cardState(item.state).label}</span>
              </button>)}
            </div>)}
        {(cursors.length > 0 || next) && <div className="row gap6">
          <Button small disabled={!cursors.length} onClick={() => { setPage(undefined); setCursors(cursors.slice(0, -1)) }}>{L('前へ')}</Button>
          <Button small disabled={!next} onClick={() => { if (next) { setPage(undefined); setCursors([...cursors, next]) } }}>{L('次へ')}</Button>
        </div>}
      </div>
    </Popover>
  </div>
}
