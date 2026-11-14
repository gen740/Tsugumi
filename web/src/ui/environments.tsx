import { useEffect, useState } from 'react'
import { L } from '../i18n'
import { EnvironmentIdentity, EnvironmentSource, RecordedEnvironment, environmentChange, environmentPage, environmentPaths, findEnvironment, storeName } from '../model/environments'
import { ProjectSession } from '../model/session'
import { R, RPCClient, s } from '../shared/rpc'
import { Button, Empty } from './controls'

export function recordedEnvironmentSource(rpc: RPCClient): EnvironmentSource {
  return {
    async environments(runId, offset) {
      const page = await rpc.call<R.BuildEnvironmentPageT>(R.RequestPayload.RunEnvironmentsRequest,
        Object.assign(new R.RunEnvironmentsRequestT(), { runId, offset, limit: 100 }))
      return { environments: page.environments.map(e => ({ id: s(e.id), manifest: s(e.manifest), derivation: s(e.derivation), system: s(e.system), pathCount: e.pathCount })), next: page.nextOffset }
    },
    async paths(runId, environmentId, offset, query, excluding) {
      const page = await rpc.call<R.EnvironmentPathPageT>(R.RequestPayload.RunEnvironmentPathsRequest,
        Object.assign(new R.RunEnvironmentPathsRequestT(), { runId, environmentId, offset, limit: 100, query, excludingRunId: excluding?.run ?? null, excludingEnvironmentId: excluding?.id ?? null }))
      return { paths: page.paths.map(s), next: page.nextOffset, total: page.total }
    },
  }
}

export function StorePath({ value }: { value: string }) {
  return <details><summary>{storeName(value)}</summary><code className="selectable wrap-pre">{value}</code></details>
}

/** Every navigation replaces the page; closures are never accumulated in memory. */
export function DependencyPaths({ session, identity, excluding }: { session: ProjectSession; identity: EnvironmentIdentity; excluding?: EnvironmentIdentity }) {
  const [query, setQuery] = useState('')
  const [offsets, setOffsets] = useState([0n])
  const [page, setPage] = useState<{ paths: string[]; next: bigint | null; total: bigint }>()
  const [error, setError] = useState<string>()
  const [attempt, setAttempt] = useState(0)
  const offset = offsets[offsets.length - 1]
  useEffect(() => {
    let alive = true
    setPage(undefined); setError(undefined)
    if (!session.rpc) { setError(L('実行環境に接続していません')); return }
    void environmentPaths(recordedEnvironmentSource(session.rpc), identity.run, identity.id, offset, query, excluding)
      .then(p => { if (alive) setPage(p) }).catch(e => { if (alive) setError(String(e.message ?? e)) })
    return () => { alive = false }
  }, [session.rpc, identity.run, identity.id, excluding?.run, excluding?.id, offset, query, attempt])
  return <section className="col gap8">
    <label>{L('依存パスを検索')}<input value={query} onChange={e => { setQuery(e.target.value); setOffsets([0n]) }} /></label>
    <p className="caption">{L('検索結果を 100 件ずつ表示します。')} {page && String(page.total)}</p>
    {error ? <p role="alert">{error} <Button onClick={() => setAttempt(a => a + 1)}>{L('再試行')}</Button></p>
      : !page ? <p>{L('読み込み中…')}</p> : page.paths.length ? page.paths.map(p => <StorePath key={p} value={p} />) : <p>{L('一致する依存パスはありません')}</p>}
    <div className="row gap8">
      <Button disabled={!page || offsets.length === 1} onClick={() => setOffsets(v => v.slice(0, -1))}>{L('前へ')}</Button>
      <span>{L('ページ')} {offsets.length}</span>
      <Button disabled={!page || page.next === null} onClick={() => { if (page?.next !== null && page?.next !== undefined) setOffsets(v => [...v, page.next!]) }}>{L('次へ')}</Button>
    </div>
  </section>
}

export async function recordedEnvironmentUses(rpc: RPCClient, runId: string, environmentId: string, offset: bigint) {
  const page = await rpc.call<R.EnvironmentUsePageT>(R.RequestPayload.RunEnvironmentUsesRequest,
    Object.assign(new R.RunEnvironmentUsesRequestT(), { runId, environmentId, offset, limit: 100 }))
  if (page.nextOffset !== null && page.nextOffset <= offset) throw Error('Environment usage pagination did not advance')
  return page
}

function EnvironmentUses({ session, runId, environmentId }: { session: ProjectSession; runId: string; environmentId: string }) {
  const [offsets, setOffsets] = useState([0n])
  const [page, setPage] = useState<R.EnvironmentUsePageT>()
  const [error, setError] = useState<string>()
  const [attempt, setAttempt] = useState(0)
  const offset = offsets.at(-1)!
  useEffect(() => {
    let alive = true
    setPage(undefined); setError(undefined)
    if (!session.rpc) { setError(L('実行環境に接続していません')); return }
    void recordedEnvironmentUses(session.rpc, runId, environmentId, offset)
      .then(p => { if (alive) setPage(p) }).catch(e => { if (alive) setError(String(e.message ?? e)) })
    return () => { alive = false }
  }, [session.rpc, runId, environmentId, offset, attempt])
  return <section><h4>{L('この環境を共有する Card と呼び出し')}</h4>
    {error ? <p role="alert">{error}<Button onClick={() => setAttempt(a => a + 1)}>{L('再試行')}</Button></p> : !page ? <p>{L('読み込み中…')}</p>
      : page.uses.map(u => <p key={`${s(u.card)}:${u.invocation}`}>{session.displayPath(s(u.card).split('/'))} · #{String(u.invocation)}</p>)}
    <Button disabled={!page || offsets.length === 1} onClick={() => setOffsets(v => v.slice(0, -1))}>{L('前へ')}</Button>
    <span> {L('ページ')} {offsets.length} </span>
    <Button disabled={!page || page.nextOffset === null} onClick={() => { if (page?.nextOffset != null) setOffsets(v => [...v, page.nextOffset!]) }}>{L('次へ')}</Button>
  </section>
}

/** Mounted by historical run and environment identity; changing selection discards old pages. */
export function EnvironmentView({ session, runId, environmentId }: { session: ProjectSession; runId: string; environmentId: string }) {
  const [environment, setEnvironment] = useState<RecordedEnvironment>()
  const [loaded, setLoaded] = useState(false)
  const [error, setError] = useState<string>()
  const [attempt, setAttempt] = useState(0)
  useEffect(() => {
    const abort = new AbortController()
    setLoaded(false); setEnvironment(undefined); setError(undefined)
    if (!session.rpc) { setError(L('実行環境に接続していません')); setLoaded(true); return }
    void findEnvironment(recordedEnvironmentSource(session.rpc), runId, environmentId, abort.signal)
      .then(e => { if (!abort.signal.aborted) setEnvironment(e) })
      .catch(e => { if (!abort.signal.aborted) setError(String(e.message ?? e)) })
      .finally(() => { if (!abort.signal.aborted) setLoaded(true) })
    return () => abort.abort()
  }, [session.rpc, runId, environmentId, attempt])
  return <section className="col gap8">
    {error && <p role="alert">{error} <Button onClick={() => setAttempt(a => a + 1)}>{L('再試行')}</Button></p>}
    {!loaded ? <p>{L('読み込み中…')}</p> : !environment && !error ? <p>{L('この呼び出しの環境は記録されていません')}</p> : null}
    {environment && <>
      <p>{L('対象システム')}: {environment.system}</p>
      <h4>{L('Manifest の出力パス')}</h4><StorePath value={environment.manifest} />
      <h4>{L('Derivation')}</h4><StorePath value={environment.derivation} />
      <EnvironmentUses key={`${runId}:${environmentId}`} session={session} runId={runId} environmentId={environmentId} />
      <h4>{L('実行時の依存パス')} ({String(environment.pathCount)})</h4>
      <p>{L('実行時の closure は、ビルド時の依存関係の完全な一覧ではありません。')}</p>
      <DependencyPaths key={`${runId}:${environmentId}`} session={session} identity={{ run: runId, id: environmentId }} />
    </>}
  </section>
}

export function RunEnvironments({ session, runId, environmentId }: { session: ProjectSession; runId: string; environmentId: string }) {
  const [selected, setSelected] = useState(environmentId)
  const [offsets, setOffsets] = useState([0n])
  const [page, setPage] = useState<{ environments: RecordedEnvironment[]; next: bigint | null }>()
  const [error, setError] = useState<string>()
  const [attempt, setAttempt] = useState(0)
  const offset = offsets.at(-1)!
  useEffect(() => {
    let alive = true
    setPage(undefined); setError(undefined)
    if (!session.rpc) { setError(L('実行環境に接続していません')); return }
    void environmentPage(recordedEnvironmentSource(session.rpc), runId, offset)
      .then(p => { if (alive) setPage(p) }).catch(e => { if (alive) setError(String(e.message ?? e)) })
    return () => { alive = false }
  }, [session.rpc, runId, offset, attempt])
  return <div className="scroll pad10 col gap8"><h3>{L('この実行の共有環境')}</h3>
    {error && <p role="alert">{error}<Button onClick={() => setAttempt(a => a + 1)}>{L('再試行')}</Button></p>}
    {!page && !error && <p>{L('読み込み中…')}</p>}
    {page?.environments.map(e => <Button key={e.id} onClick={() => setSelected(e.id)} pressed={selected === e.id}>{storeName(e.manifest)} · {e.system} · {e.id.slice(0, 8)}</Button>)}
    <div><Button disabled={!page || offsets.length === 1} onClick={() => setOffsets(v => v.slice(0, -1))}>{L('前へ')}</Button>
      <span> {L('ページ')} {offsets.length} </span>
      <Button disabled={!page || page.next === null} onClick={() => { if (page?.next != null) setOffsets(v => [...v, page.next!]) }}>{L('次へ')}</Button></div>
    <EnvironmentView key={`${runId}:${selected}`} session={session} runId={runId} environmentId={selected} />
  </div>
}

/** No definition is inspected, including when the selected invocation has no environment. */
export function InvocationPackages({ session, runId, environmentId }: { session: ProjectSession; runId: string; environmentId: string }) {
  if (!environmentId) return <Empty title={L('この呼び出しの環境は記録されていません')} icon="shippingbox"
    description={L('依存 package は実行時に記録されたものだけを表示します。現在の Nix 定義からは求めません。')} />
  return <RunEnvironments key={`${runId}:${environmentId}`} session={session} runId={runId} environmentId={environmentId} />
}

export function EnvironmentComparison({ session, left, right }: { session: ProjectSession; left: EnvironmentIdentity; right: EnvironmentIdentity }) {
  const difference = environmentChange(left.id, right.id)
  return <section className={difference === 'changed' ? 'comparison-field changed' : 'comparison-field'}>
    <h4>{L('環境')} · {difference === 'unknown' ? L('比較できません') : difference === 'changed' ? L('環境が変わった Card') : L('同じ環境です')}</h4>
    {difference === 'unknown' ? <p>{L('この呼び出しの環境は記録されていません')}</p> : difference === 'changed' && <>
      <p>{L('実行時の closure は、ビルド時の依存関係の完全な一覧ではありません。')}</p>
      <div className="comparison-pair">
        <div><h4>{L('削除された実行時の依存パス')}</h4><DependencyPaths session={session} identity={left} excluding={right} /></div>
        <div><h4>{L('追加された実行時の依存パス')}</h4><DependencyPaths session={session} identity={right} excluding={left} /></div>
      </div>
    </>}
  </section>
}
