import { useEffect, useState } from 'react'
import { L } from '../i18n'
import { EnvironmentSource, RecordedEnvironment, environmentPaths, findEnvironment, storeName } from '../model/environments'
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
    async paths(runId, environmentId, offset, query) {
      const page = await rpc.call<R.EnvironmentPathPageT>(R.RequestPayload.RunEnvironmentPathsRequest,
        Object.assign(new R.RunEnvironmentPathsRequestT(), { runId, environmentId, offset, limit: 100, query }))
      return { paths: page.paths.map(s), next: page.nextOffset, total: page.total }
    },
  }
}

function StorePath({ value }: { value: string }) {
  // A selectable full path is also usable when clipboard permission is unavailable.
  return <details><summary>{storeName(value)}</summary><code className="selectable wrap-pre">{value}</code></details>
}

/** Mounted by historical run and environment identity; changing selection discards old pages. */
export function EnvironmentView({ session, runId, environmentId }: { session: ProjectSession; runId: string; environmentId: string }) {
  const [environment, setEnvironment] = useState<RecordedEnvironment>()
  const [loaded, setLoaded] = useState(false)
  const [error, setError] = useState<string>()
  const [attempt, setAttempt] = useState(0)
  const [query, setQuery] = useState('')
  const [offsets, setOffsets] = useState([0n])
  const [paths, setPaths] = useState<string[]>([])
  const [next, setNext] = useState<bigint | null>(null)
  const [busy, setBusy] = useState(false)
  const offset = offsets[offsets.length - 1]
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
  useEffect(() => {
    let alive = true
    setPaths([]); setNext(null)
    if (!environment || !session.rpc) return
    setBusy(true); setError(undefined)
    void environmentPaths(recordedEnvironmentSource(session.rpc), runId, environment.id, offset, query)
      .then(p => { if (alive) { setPaths(p.paths); setNext(p.next) } })
      .catch(e => { if (alive) setError(String(e.message ?? e)) })
      .finally(() => { if (alive) setBusy(false) })
    return () => { alive = false }
  }, [session.rpc, environment, runId, offset, query, attempt])
  return <section className="scroll pad10 col gap8">
    <h3>{L('依存 package')}</h3>
    {error && <p role="alert">{error} <Button onClick={() => setAttempt(a => a + 1)}>{L('再試行')}</Button></p>}
    {!loaded ? <p>{L('読み込み中…')}</p> : !environment && !error ? <p>{L('この呼び出しの環境は記録されていません')}</p> : null}
    {environment && <>
      <p>{environment.system}</p>
      <h4>{L('実行時の依存パス')} ({String(environment.pathCount)})</h4>
      <p>{L('この呼び出しで記録されたものです。ビルド時だけに使う依存関係は含まれません。')}</p>
      <label>{L('依存パスを検索')}<input value={query} onChange={e => { setQuery(e.target.value); setOffsets([0n]) }} /></label>
      <p className="caption">{L('検索結果を 100 件ずつ表示します。')}</p>
      {busy ? <p>{L('読み込み中…')}</p> : paths.length ? paths.map(p => <StorePath key={p} value={p} />) : <p>{L('一致する依存パスはありません')}</p>}
      <div className="row gap8">
        <Button disabled={busy || offsets.length === 1} onClick={() => setOffsets(v => v.slice(0, -1))}>{L('前へ')}</Button>
        <span>{L('ページ')} {offsets.length}</span>
        <Button disabled={busy || next === null} onClick={() => { if (next !== null) setOffsets(v => [...v, next]) }}>{L('次へ')}</Button>
      </div>
    </>}
  </section>
}

/** The packages of one invocation in the run history. An invocation without a
 *  recorded environment says so: nothing is derived from the current definition. */
export function InvocationPackages({ session, runId, environmentId }: { session: ProjectSession; runId: string; environmentId: string }) {
  if (!environmentId) {
    return <Empty title={L('この呼び出しの環境は記録されていません')} icon="shippingbox"
      description={L('依存 package は実行時に記録されたものだけを表示します。現在の Nix 定義からは求めません。')} />
  }
  return <EnvironmentView key={`${runId}:${environmentId}`} session={session} runId={runId} environmentId={environmentId} />
}
