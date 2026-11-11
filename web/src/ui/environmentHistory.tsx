import { ReactNode, useEffect, useState } from 'react'
import { L } from '../i18n'
import { CardEnvironment, compareCardEnvironments, groupCardEnvironments, storeName } from '../model/environments'
import { ProjectSession, formatMillis } from '../model/session'
import { R, s, segs } from '../shared/rpc'
import { Button } from './controls'
import { EnvironmentView } from './environments'

function LazyDetail({ label, children }: { label: string; children: ReactNode }) {
  const [open, setOpen] = useState(false)
  return <details onToggle={e => setOpen(e.currentTarget.open)}><summary>{label}</summary>{open && children}</details>
}

type Ref = { run: string; id: string }
function DependencyDifference({ session, source, excluded }: { session: ProjectSession; source: Ref; excluded?: Ref }) {
  const [query, setQuery] = useState('')
  const [offsets, setOffsets] = useState([0n])
  const [page, setPage] = useState<R.EnvironmentPathPageT>()
  const [error, setError] = useState<string>()
  const [attempt, setAttempt] = useState(0)
  const offset = offsets[offsets.length - 1]
  useEffect(() => {
    let alive = true
    setPage(undefined); setError(undefined)
    if (!session.rpc) { setError(L('実行環境に接続していません')); return }
    void session.rpc.call<R.EnvironmentPathPageT>(R.RequestPayload.RunEnvironmentPathsRequest,
      Object.assign(new R.RunEnvironmentPathsRequestT(), { runId: source.run, environmentId: source.id,
        excludeRunId: excluded?.run ?? null, excludeEnvironmentId: excluded?.id ?? null, query, offset, limit: 100 }))
      .then(p => { if (alive) setPage(p) }).catch(e => { if (alive) setError(String(e.message ?? e)) })
    return () => { alive = false }
  }, [session.rpc, source.run, source.id, excluded?.run, excluded?.id, query, offset, attempt])
  return <div>
    <label>{L('依存パスを検索')}<input value={query} onChange={e => { setQuery(e.target.value); setOffsets([0n]) }} /></label>
    {error ? <p role="alert">{error}<Button onClick={() => setAttempt(v => v + 1)}>{L('再試行')}</Button></p>
      : !page ? <p>{L('読み込み中…')}</p> : <>
        <p>{String(page.total)} {L('件')}</p>
        {page.paths.map(p => <details key={s(p)}><summary>{storeName(s(p))}</summary><code className="selectable wrap-pre">{s(p)}</code></details>)}
        <Button disabled={offsets.length === 1} onClick={() => setOffsets(v => v.slice(0, -1))}>{L('前へ')}</Button>
        <Button disabled={page.nextOffset === null} onClick={() => { if (page.nextOffset !== null) setOffsets(v => [...v, page.nextOffset!]) }}>{L('次へ')}</Button>
      </>}
  </div>
}

/** Run-wide view groups shared environments and compares latest invocations by Card path. */
export function EnvironmentHistory({ session, runId }: { session: ProjectSession; runId: string }) {
  const [rows, setRows] = useState<CardEnvironment[]>([])
  const [before, setBefore] = useState<CardEnvironment[]>([])
  const [runs, setRuns] = useState<R.RunSummaryT[]>([])
  const [cursor, setCursor] = useState<string | null>(null)
  const [loadingRuns, setLoadingRuns] = useState(false)
  const [compare, setCompare] = useState('')
  const [selected, setSelected] = useState('')
  const [error, setError] = useState<string>()
  const [busy, setBusy] = useState(false)
  const [attempt, setAttempt] = useState(0)
  useEffect(() => {
    let alive = true
    setBusy(true); setError(undefined); setRows([]); setBefore([])
    if (!session.rpc) { setError(L('実行環境に接続していません')); setBusy(false); return }
    const rpc = session.rpc
    const latest = async (run: string) => (await rpc.call<R.InvocationPageT>(R.RequestPayload.RunLatestInvocationsRequest,
      Object.assign(new R.RunLatestInvocationsRequestT(), { runId: run }))).records.map(r => ({ card: segs(r.card).join('/'), environment: s(r.environmentId) || undefined }))
    void Promise.all([latest(runId), compare ? latest(compare) : Promise.resolve([])]).then(([current, old]) => {
      if (alive) { setRows(current); setBefore(old) }
    }).catch(e => { if (alive) setError(String(e.message ?? e)) }).finally(() => { if (alive) setBusy(false) })
    return () => { alive = false }
  }, [session.rpc, runId, compare, attempt])
  useEffect(() => {
    let alive = true
    if (!session.rpc) return
    void session.rpc.history(null, false, 100).then(p => { if (alive) { setRuns(p.runs); setCursor(p.nextCursor === null ? null : s(p.nextCursor)) } })
      .catch(e => { if (alive) setError(String(e.message ?? e)) })
    return () => { alive = false }
  }, [session.rpc, attempt])
  const labels: Record<string, string> = { added: L('追加された Card'), removed: L('削除された Card'), changed: L('環境が変わりました'), same: L('同じ環境です'), unrecorded: L('環境の記録がありません') }
  return <section className="scroll pad10 col gap8">
    <h3>{L('実行全体の環境')}</h3><p>{L('各 Card の最後の呼び出しを比較します。')}</p>
    <label>{L('比較元の実行')}<select value={compare} onChange={e => { setCompare(e.target.value); setSelected('') }}>
      <option value="">{L('比較しない')}</option>
      {runs.map(r => <option key={s(r.status!.runId)} value={s(r.status!.runId)}>{formatMillis(r.status!.acceptedAt)} · {s(r.status!.runId)}</option>)}
    </select></label>
    {cursor && <Button disabled={loadingRuns} onClick={() => {
      if (!session.rpc || loadingRuns) return
      setLoadingRuns(true)
      void session.rpc.history(null, false, 100, cursor).then(p => { setRuns(v => [...v, ...p.runs]); setCursor(p.nextCursor === null ? null : s(p.nextCursor)) }).catch(e => setError(String(e.message ?? e))).finally(() => setLoadingRuns(false))
    }}>{L('以前の実行を読み込む')}</Button>}
    {error && <p role="alert">{error}<Button onClick={() => setAttempt(v => v + 1)}>{L('再試行')}</Button></p>}
    {busy ? <p>{L('読み込み中…')}</p> : compare ? compareCardEnvironments(before, rows).map(row => <LazyDetail key={`${compare}:${runId}:${row.card}`} label={`${row.card} · ${labels[row.state]}`}>
      {row.after && (row.state === 'changed' || row.state === 'added') && <>
        <h4>{L('追加された依存パス')}</h4><DependencyDifference key={`added:${compare}:${runId}:${row.card}`} session={session} source={{ run: runId, id: row.after }} excluded={row.before ? { run: compare, id: row.before } : undefined} />
      </>}
      {row.before && (row.state === 'changed' || row.state === 'removed') && <>
        <h4>{L('削除された依存パス')}</h4><DependencyDifference key={`removed:${compare}:${runId}:${row.card}`} session={session} source={{ run: compare, id: row.before }} excluded={row.after ? { run: runId, id: row.after } : undefined} />
      </>}
      {row.before && <LazyDetail label={L('比較元の実行')}><EnvironmentView key={`${compare}:${row.before}`} session={session} runId={compare} environmentId={row.before} /></LazyDetail>}
      {row.after && <LazyDetail label={L('記録された実行環境')}><EnvironmentView key={`${runId}:${row.after}`} session={session} runId={runId} environmentId={row.after} /></LazyDetail>}
    </LazyDetail>) : groupCardEnvironments(rows).map(group => <div key={group.id}>
      <Button onClick={() => setSelected(group.id)} disabled={!group.id}>{group.id || L('環境の記録がありません')}</Button>
      <p>{group.cards.join(', ')}</p>
    </div>)}
    {selected && !compare && <EnvironmentView key={`${runId}:${selected}`} session={session} runId={runId} environmentId={selected} />}
  </section>
}
