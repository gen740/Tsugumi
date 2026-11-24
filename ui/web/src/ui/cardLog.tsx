// The Card window's Log and Dependencies categories.
import { useEffect, useState } from 'react'
import { L } from '../i18n'
import { nixReferences } from '../model/nixReferences'
import { useObserve } from '../model/observable'
import { errorText, formatMillis, path, ProjectSession } from '../model/session'
import { R, s } from '../wire/rpc'
import type { CardEditorModel } from './cardSettings'
import { Button, Empty, Spinner } from './controls'
import { EnvironmentView } from './environments'
import { Icon } from './icons'
import { StageLogView } from './logs'

/** The latest invocation of this Card (the running one while it runs), or the
 *  one opened from the execution history. */
export function CardLogView({ session, model }: { session: ProjectSession; model: CardEditorModel }) {
  useObserve(session, model)
  const cardKey = model.cardKey
  const [latest, setLatest] = useState<R.RunSummaryT>()
  const [loaded, setLoaded] = useState(false)
  const [error, setError] = useState<string>()
  const inActiveRun = session.runActive && session.cardStates.has(cardKey)
  useEffect(() => {
    const rpc = session.rpc
    if (!rpc) {
      setError(L('実行環境に接続していません'))
      setLoaded(true)
      return
    }
    let alive = true
    void rpc.history(path(cardKey), false, 1).then((p) => {
      if (!alive) return
      setLatest(p.runs[0])
      setError(undefined)
    }).catch((e) => alive && setError(errorText(e))).finally(() => alive && setLoaded(true))
    return () => { alive = false }
  }, [session.rpc, session.lastFinished, session.currentRunId, cardKey])
  const selection = model.logSelection
  const shown = selection ?? (inActiveRun && session.currentRunId ? { runId: session.currentRunId, invocation: undefined }
    : latest ? { runId: s(latest.status!.runId), invocation: undefined } : undefined)
  const runTime = (id: string) => id === s(latest?.status?.runId) ? formatMillis(latest?.status?.acceptedAt) : id
  const caption = selection
    ? L('実行履歴から開いた呼び出し: {0} · {1}', runTime(selection.runId), selection.invocation !== undefined ? L('{0} 回目', String(selection.invocation)) : L('最後の呼び出し'))
    : inActiveRun ? L('進行中の実行（最新の呼び出しを追います）')
    : latest ? L('最後の実行: {0}', formatMillis(latest.status!.acceptedAt)) : ''
  return (
    <div className="col fill min0">
      <div className="row gap8 center pad6 border-bottom">
        <Icon name={selection ? 'pin' : inActiveRun ? 'play.circle' : 'clock'} className="secondary" />
        <span className="callout ellipsis flex1">{caption}</span>
        {selection && <Button small onClick={() => model.update((m) => { m.logSelection = undefined })}>{L('最新の呼び出しに戻る')}</Button>}
        <Button small onClick={() => model.update((m) => { m.category = 'executions' })}>{L('実行履歴')}</Button>
      </div>
      {shown ? <StageLogView session={session} runId={shown.runId} card={path(cardKey)} invocation={shown.invocation} />
        : !loaded ? <div className="center-fill"><Spinner /></div>
        : error ? <Empty title={L('履歴を読み込めませんでした')} icon="exclamationmark.triangle" description={error} />
        : <Empty title={L('まだ実行していません')} icon="text.alignleft" description={L('この Card が実行されると、ここに段階ごとのログが出ます。実行中は出力をそのまま追います。')} />}
    </div>
  )
}

/** What a Card depends on is part of its definition: default.nix chooses the
 *  SDK, the nixpkgs packages and the other Cards it takes outputs from. The
 *  packages are shown as the last invocation recorded them; the current
 *  definition is not evaluated for this. */
export function DependenciesCategory({ model }: { model: CardEditorModel }) {
  const session = model.session
  useObserve(session, model)
  const [latest, setLatest] = useState<{ runId: string; acceptedAt: bigint | null; invocation: bigint; environmentId: string }>()
  const [loaded, setLoaded] = useState(false)
  useEffect(() => {
    const rpc = session.rpc
    if (!rpc) return setLoaded(true)
    let alive = true
    void (async () => {
      const runs = (await rpc.history(path(model.cardKey), false, 10).catch(() => undefined))?.runs ?? []
      for (const run of runs) {
        const runId = s(run.status!.runId)
        const record = await rpc.record(runId, path(model.cardKey), null, true).catch(() => undefined)
        if (!alive) return
        const id = s(record?.record?.summary?.environmentId)
        if (id) {
          setLatest({ runId, acceptedAt: run.status!.acceptedAt, invocation: record?.selectedInvocation ?? 0n, environmentId: id })
          return
        }
      }
      setLatest(undefined)
    })().finally(() => alive && setLoaded(true))
    return () => { alive = false }
  }, [session.rpc, session.lastFinished, model.cardKey])
  const hasDefinition = model.files.some((f) => f.endsWith('/default.nix'))
  const refs = nixReferences(session).filter((r) => r.source === model.cardKey)
  return (
    <div className="scroll pad12 col gap12">
      <div className="callout secondary">{L('この Card が使う SDK、nixpkgs の package、上流 Card の Nix exports は、Nix 定義（default.nix）で決まります。変えるときは default.nix を編集します。')}</div>
      <div className="row gap8 center">
        {hasDefinition ? <Button onClick={() => model.openDefinition()}>{L('default.nix を開く')}</Button>
          : model.kind === 'code' && <span className="secondary">{L('default.nix がありません')}</span>}
      </div>
      <h3>{L('参照している Card')}</h3>
      {!refs.length ? <div className="secondary">{L('default.nix はほかの Card を参照していません。')}</div> : refs.map((ref) => (
        <div key={ref.id} className="col gap4">
          <span>{session.displayPath(path(ref.target))}</span>
          <code className="caption secondary selectable">{ref.expression}</code>
        </div>
      ))}
      <h3>{L('最後に記録された環境')}</h3>
      {!loaded ? <Spinner /> : latest ? <>
        <div className="caption secondary">{L('実行 {0} の {1} 回目で使われた環境です。default.nix を変えた後は、次に実行したときに変わります。', formatMillis(latest.acceptedAt), String(latest.invocation))}</div>
        <EnvironmentView key={`${latest.runId}:${latest.environmentId}`} session={session} runId={latest.runId} environmentId={latest.environmentId} />
      </> : <div className="secondary">{L('まだ記録された環境はありません。実行すると、使った環境がここに出ます。')}</div>}
    </div>
  )
}
