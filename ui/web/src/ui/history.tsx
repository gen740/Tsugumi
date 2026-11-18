// Run history, artifacts and the enlarged output display of one Card.
import { useEffect, useMemo, useState } from 'react'
import { app } from '../model/app'
import { artifactHTML } from '../model/artifactHTML'
import { useObserve } from '../model/observable'
import { CardKey, cardState, errorText, formatBytes, formatDuration, formatMillis, path, ProjectSession, remoteExecutionLabel, runState } from '../model/session'
import { bool } from '../../shared/json'
import { describeValue } from '../wire/flatjson'
import { D, R, s, segs, sOpt } from '../wire/rpc'
import { Button, download, Empty, Form, HSplit, Row, Section, Segmented, Select, Sheet, Spinner, TextField, Value } from './controls'
import { RunComparisonView } from './runComparison'
import { useBatchHistoryRevision } from './batchInvocations'
import { ArtifactHTMLPreview } from './artifactHTMLPreview'
import { DisplayContent } from './display'
import { Icon } from './icons'
import type { CardEditorModel } from './cardSettings'
import { L } from '../i18n'
import { runMark } from '../model/runMark'

export function FailureSourceLink({ failure, onOpenSource }: { failure: D.FailureT; onOpenSource: (source: string, line: number) => void }) {
  const args = new Map(failure.args.map((a) => [s(a.key), s(a.value)]))
  const source = args.get('source'), line = Number(args.get('line'))
  if (!source || !Number.isSafeInteger(line) || line < 1) return null
  return <Row><Button onClick={() => onOpenSource(source, line)}>{args.get('cell') ?? source} · {source}:{line}</Button></Row>
}

type Filter = 'all' | 'succeeded' | 'failed'

/** Which call of the Card an invocation is, with its state, list position and time. */
export function invocationLabel(i: R.InvocationSummaryT): string {
  return L('{0} 回目', String(i.invocation ?? 0n)) + ` · ${cardState(i.state).label}` +
    // A call of an execution list says which element it took.
    (i.batchIndex != null && i.batchSize != null && i.batchSize > 1 ? ' · ' + L('実行リスト {0}/{1}', i.batchIndex, i.batchSize) : '') +
    (i.durationMs !== null ? ` · ${formatDuration(Number(i.durationMs) / 1000)}` : '')
}

const INVOCATION_HELP = () => L('Card は入力がそろうたびに 1 回呼び出されます（実行リストなら要素ごと）。ほかの実行の呼び出しは左の一覧で実行を選びます。')

/** Runs of one Card (filter by success/failure) → invocations → what one
 *  invocation was (information first) and what it took and gave. Its log is
 *  in the Log category; its environment comes from the Card's definition.
 *  Loading, failed and empty are said plainly, never as an empty split view. */
export function CardExecutionView({ session, model }: { session: ProjectSession; model: CardEditorModel }) {
  useObserve(session)
  const cardKey = model.cardKey
  const onOpenSource = (source: string, line: number) => model.openSource(source, line)
  const [runs, setRuns] = useState<R.RunSummaryT[]>([])
  const [filter, setFilter] = useState<Filter>('all')
  const [runId, setRunId] = useState<string>()
  const [invocations, setInvocations] = useState<R.InvocationSummaryT[]>([])
  const [batches, setBatches] = useState<R.RunBatchT[]>([])
  const [cursors, setCursors] = useState<string[]>([])
  const [nextCursor, setNextCursor] = useState<string>()
  const [invocation, setInvocation] = useState<bigint>()
  const [record, setRecord] = useState<R.RecordResultT>()
  const [error, setError] = useState<string>()
  const [loaded, setLoaded] = useState(false)
  const [attempt, setAttempt] = useState(0)
  const [comparing, setComparing] = useState(false)
  const batchRevision = useBatchHistoryRevision(session)

  useEffect(() => {
    const rpc = session.rpc
    if (!rpc) {
      setError(L('実行環境に接続していません'))
      setLoaded(true)
      return
    }
    let alive = true
    void rpc.history(path(cardKey), false, 100).then((p) => {
      if (!alive) return
      setRuns(p.runs)
      setRunId((cur) => (cur && p.runs.some((r) => s(r.status!.runId) === cur) ? cur : p.runs[0] ? s(p.runs[0].status!.runId) : undefined))
      setError(undefined)
    }).catch((e) => alive && setError(L('履歴を読み込めません: {0}', errorText(e)))).finally(() => alive && setLoaded(true))
    return () => {
      alive = false
    }
  }, [session.lastFinished, session.currentRunId, session.remoteHistoryRevision, session.rpc, cardKey, attempt])
  // Another run: its own latest call is shown, never the previous run's number.
  useEffect(() => {
    setInvocation(undefined)
    setRecord(undefined)
    setInvocations([])
    setBatches([])
    setCursors([])
    setNextCursor(undefined)
  }, [runId, cardKey])
  const cursor = cursors.at(-1)
  useEffect(() => {
    const rpc = session.rpc
    if (!rpc || !runId) return setInvocations([])
    let alive = true
    void rpc.invocations(runId, path(cardKey), cursor ?? null).then((p) => {
      if (!alive) return
      setInvocations(p.records)
      setBatches(p.batches)
      setNextCursor(sOpt(p.nextCursor))
      setInvocation((current) => p.records.some((item) => item.invocation === current) ? current : p.records.at(-1)?.invocation ?? undefined)
    }).catch(() => alive && setInvocations([]))
    return () => {
      alive = false
    }
  }, [runId, cardKey, cursor, session.rpc, session.remoteHistoryRevision, batchRevision])
  useEffect(() => {
    const rpc = session.rpc
    if (!rpc || !runId) return setRecord(undefined)
    let alive = true
    void rpc.record(runId, path(cardKey), invocation ?? null, invocation === undefined)
      .then((r) => alive && setRecord(r)).catch(() => alive && setRecord(undefined))
    return () => {
      alive = false
    }
  }, [runId, invocation, cardKey, session.rpc, session.remoteHistoryRevision, batchRevision])

  if (!loaded) return <div className="center-fill col gap8"><Spinner /><span className="secondary">{L('履歴を読み込んでいます…')}</span></div>
  if (!runs.length && error) {
    return (
      <Empty title={L('履歴を読み込めませんでした')} icon="exclamationmark.triangle" description={error}>
        <Button onClick={() => setAttempt(attempt + 1)}>{L('再試行')}</Button>
      </Empty>
    )
  }
  if (!runs.length) return <Empty title={L('まだ実行していません')} icon="clock" description={L('この Card が実行されると、ここに実行ごとの情報と入出力が残ります。')} />

  const filtered = runs.filter((r) => {
    const st = r.status!.state
    return filter === 'all' || (filter === 'succeeded' ? st === D.RunState.succeeded : st === D.RunState.failed || st === D.RunState.cancelled)
  })
  const rec = record?.record
  const summary = rec?.summary ?? invocations.find((item) => item.invocation === invocation)
  const batch = summary?.batchActivation != null ? batches.find((item) => item.activation === summary.batchActivation) : batches.at(-1)
  const executionState = remoteExecutionLabel(summary?.executionState)
  const interruptedRunId = s(record?.run?.status?.runId)
  const interruptedRun = !!runId && interruptedRunId === runId && !!summary?.executionJobId &&
    record?.run?.status?.failure?.kind === D.FailureKind.interrupted
  const waiting = session.runActive && runId === session.currentRunId && !invocations.length && !rec
  return (
    <div className="execution-history fill">
    <HSplit sizes={[240, null]} min={[220, 380]}>
      {[
        <div key="runs" className="col fill">
          {runs.length > 1 && <div className="pad6"><Button onClick={() => setComparing(true)}>{L('実行を比較')}</Button></div>}
          {comparing && <Sheet width={1200} onClose={() => setComparing(false)}><RunComparisonView session={session} runs={runs} initial={runId} onClose={() => setComparing(false)} /></Sheet>}
          <div className="pad6"><Segmented<Filter> small value={filter} onChange={setFilter} options={[{ value: 'all', label: L('すべて') }, { value: 'succeeded', label: L('成功') }, { value: 'failed', label: L('失敗') }]} /></div>
          <div className="list flex1" role="listbox" aria-label={L('実行')}>
            {!filtered.length && <Empty small title={runs.length ? L('条件に合う実行はありません') : L('実行履歴はありません')} icon="clock" />}
            {filtered.map((r) => {
              const id = s(r.status!.runId), rs = runState(r.status!.state)
              return (
                <div key={id} role="option" aria-selected={id === runId} className={`list-row clickable${id === runId ? ' selected' : ''}`} onClick={() => setRunId(id)}>
                  <Icon name={rs.icon} style={{ color: id === runId ? undefined : `var(--${rs.color})` }} />
                  <div className="flex1 min0">
                    <div className="callout">{formatMillis(r.status!.acceptedAt)}</div>
                    {runMark(r) && <div className="caption">{runMark(r)}</div>}
                    <div className="caption secondary ellipsis">{rs.label} · 対象 {session.displayPath(segs(r.target))}</div>
                  </div>
                </div>
              )
            })}
          </div>
        </div>,
        <div key="detail" className="col fill">
          {!runId ? <Empty title={L('実行を選んでください')} icon="list.bullet.rectangle" /> : (
            <>
              <div className="history-detail-header row gap8 center pad6 border-bottom">
                <div className="history-invocation row gap8 center min0" title={INVOCATION_HELP()}>
                  {waiting ? <span className="ellipsis secondary" title={L('呼び出しの記録を待っています')}>{L('呼び出しの記録を待っています')}</span>
                    : invocations.length <= 1 && !cursors.length && !nextCursor ? (
                      <span className="ellipsis"><span className="secondary">{L('呼び出し')} </span>
                        {invocations[0] ? `${invocationLabel(invocations[0])} · ${L('この実行で 1 回だけ')}` : L('呼び出しなし')}</span>
                    ) : <>
                      <Select label={L('呼び出し')} style={{ maxWidth: 320 }} value={invocation === undefined ? '' : String(invocation)}
                        onChange={(v) => setInvocation(v ? BigInt(v) : undefined)}
                        options={invocations.map((i) => ({ value: String(i.invocation ?? 0n), label: invocationLabel(i) }))} />
                      <span className="caption secondary nowrap">{cursors.length || nextCursor ? L('500 回ずつ') : L('全 {0} 回', invocations.length)}</span>
                    </>}
                  {batch?.executionList && <span className="caption mono" title={L('実行リストの要素数')}>×{batch.invocationCount}</span>}
                  {(cursors.length > 0 || nextCursor) && <>
                    <Button small disabled={!cursors.length} help={L('前の 500 回')} onClick={() => { setNextCursor(undefined); setCursors(cursors.slice(0, -1)) }}>{L('前へ')}</Button>
                    <Button small disabled={!nextCursor} help={L('次の 500 回')} onClick={() => { if (nextCursor) { setCursors([...cursors, nextCursor]); setNextCursor(undefined) } }}>{L('次へ')}</Button>
                  </>}
                </div>
                <span className="spacer" />
                <Button small disabled={record?.exists === false} help={L('この呼び出しのログを「ログ」で開きます')} onClick={() => model.openLog(runId, invocation)}>{L('ログ')}</Button>
              </div>
              <div className="flex1 col min0">
                {waiting ? <div className="pad10 secondary ellipsis" title={L('呼び出しが記録されると表示します。')}>{L('呼び出しが記録されると表示します。')}</div>
                  : record?.exists === false ? <Empty title={L('この実行では呼び出されていません')} icon="circle.dotted" description={L('実行の対象に含まれなかったか、入力がそろう前に実行が終わりました。')} />
                  : !record ? <div className="center-fill"><Spinner /></div> : (
                  <div className="scroll">
                    <Form>
                      <Section title={L('情報')}>
                        <Value label={L('状態')} value={cardState(record.status!.state).label} />
                        {runMark(record.run) && <Value label={L('実行の種類')} value={runMark(record.run)} />}
                        {summary && (
                          <>
                            <Value label={L('呼び出し')} value={(record.aggregate?.calls ?? 0n) > 1n
                              ? L('{0} 回目（この実行で {1} 回）', String(summary.invocation ?? 0n), String(record.aggregate?.calls ?? 0n))
                              : L('{0} 回目（この実行で 1 回だけ）', String(summary.invocation ?? 0n))} />
                            <Value label={L('段階')} value={D.Phase[summary.phase] ?? ''} />
                            <Value label={L('開始')} value={formatMillis(summary.startedAt)} />
                            <Value label={L('終了')} value={formatMillis(summary.endedAt)} />
                            {summary.durationMs !== null && <Value label={L('所要時間')} value={formatDuration(Number(summary.durationMs) / 1000)} />}
                            {summary.executionService && <Value label={L('実行サービス')} value={s(summary.executionService) === 'local' ? L('プロジェクトの実行環境') : s(summary.executionService)} />}
                            {executionState && <Value label={L('実行サービスの状態')} value={executionState} />}
                            {summary.executionSystem && <Value label={L('CPU アーキテクチャ')} value={s(summary.executionSystem)} />}
                            {summary.executionJobId && <Value label={L('ジョブ ID')} value={s(summary.executionJobId)} />}
                            {interruptedRun && (
                              <Row><span className="caption secondary">{L('Tsugumi の再起動により、この実行は中断されました。結果は回収されません。必要な Card を改めて実行してください。')}</span></Row>
                            )}
                            {summary.executionResources?.cpuMillis != null && <Value label={L('CPU 上限（ミリコア）')} value={String(summary.executionResources.cpuMillis)} />}
                            {summary.executionResources?.memoryBytes != null && <Value label={L('メモリ上限（バイト）')} value={String(summary.executionResources.memoryBytes)} />}
                            {/* A Card built for another system (its `system` setting): what ran it. */}
                            {summary.emulator && <Value label={L('エミュレーター')} value={s(summary.emulator)} />}
                            {summary.waitingInputs.length > 0 && <Value label={L('待っていた入力')} value={summary.waitingInputs.map((w) => s(w.port)).join(', ')} />}
                          </>
                        )}
                        {record.status!.failure && (
                          <>
                            <Value label={L('失敗の種類')} value={D.FailureKind[record.status!.failure.kind] ?? String(record.status!.failure.kind)} />
                            <Row><span className="selectable wrap-pre" style={{ color: 'var(--red)' }}>{s(record.status!.failure.diagnostic)}</span></Row>
                            <FailureSourceLink failure={record.status!.failure} onOpenSource={onOpenSource} />
                          </>
                        )}
                        <Value label={L('この実行での集計')} value={L('呼び出し {0} · 成功 {1} · 失敗 {2}', record.aggregate?.calls ?? 0n, record.aggregate?.succeeded ?? 0n, record.aggregate?.failed ?? 0n)} />
                        {rec?.inputSources.map((x, i) => <Value key={i} label={L('入力元 {0}', s(x.port))} value={`${session.displayPath(segs(x.card))} · ${L('{0} 回目', String(x.invocation ?? 0n))}`} />)}
                        {record.orphaned && <Row><span className="secondary">{L('この Card は文書から削除されています。')}</span></Row>}
                      </Section>
                      {/* Not recorded is a setting of this Card, said as such: the section stays, greyed. */}
                      {rec?.recordIo === false || record.recordingDisabled ? (
                        <Section title={<span className="history-section-off">{L('入出力（記録していません）')}</span>}>
                          <div className="history-io-off col gap8 pad10">
                            <span className="label secondary"><Icon name="nosign" />{L('この Card は入出力を記録しない設定です。値は残っていません。')}</span>
                            <Row><Button onClick={() => model.update((m) => { m.category = 'ports' })}>{L('入出力の設定を開く')}</Button></Row>
                          </div>
                        </Section>
                      ) : rec ? (
                        <>
                          <Section title={L('入力')}>
                            {!rec.inputs.length && <Row><span className="secondary">{L('なし')}</span></Row>}
                            {rec.inputs.map((v, i) => <ValueBox key={i} v={v} />)}
                          </Section>
                          <Section title={L('出力')}>
                            {!rec.outputs.length && <Row><span className="secondary">{L('なし')}</span></Row>}
                            {rec.outputs.map((v, i) => <ValueBox key={i} v={v} />)}
                          </Section>
                        </>
                      ) : <Section title={L('入出力')}><Row><span className="secondary">{L('この呼び出しの記録はまだありません。')}</span></Row></Section>}
                    </Form>
                  </div>
                )}
              </div>
            </>
          )}
          {error && <div className="caption pad6" style={{ color: 'var(--red)' }}>{error}</div>}
        </div>,
      ]}
    </HSplit>
    </div>
  )
}

/** A recorded input/output value as pretty JSON (decoded with its own BFBS). */
function ValueBox({ v }: { v: D.NamedValueT }) {
  const text = useMemo(() => describeValue({
    name: s(v.name), rootType: s(v.rootType), sizeBytes: v.sizeBytes, artifactId: v.artifactId === null ? null : s(v.artifactId),
    filePath: v.filePath === null ? null : s(v.filePath), fileMissing: v.fileMissing, inlineBytes: v.inlineBytes, schemaBfbs: v.schemaBfbs,
  }), [v])
  return <pre className="value-box selectable">{text}</pre>
}

type Preview = { kind: 'html'; html: string } | { kind: 'text'; text: string } | { kind: 'image'; url: string } | { kind: 'binary'; size: number }
const IMAGE_TYPES: Record<string, string> = {
  png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif', tiff: 'image/tiff', svg: 'image/svg+xml', webp: 'image/webp', pdf: 'application/pdf',
}
const kindName = (k: D.PortKind) => (k === D.PortKind.File ? 'file' : k === D.PortKind.Directory ? 'directory' : k === D.PortKind.FlatBuffer ? 'flatbuffer' : 'unspecified')

/** Retained outputs of one Card: run/invocation → outputs → files → preview. */
export function CardArtifactsView({ session, cardKey }: { session: ProjectSession; cardKey: CardKey }) {
  useObserve(session)
  const [runs, setRuns] = useState<R.RunSummaryT[]>([])
  const [runId, setRunId] = useState<string>()
  const [artifacts, setArtifacts] = useState<R.ArtifactInfoT[]>([])
  const [artifactId, setArtifactId] = useState<string>()
  const [member, setMember] = useState<string>()
  const [search, setSearch] = useState('')
  const [preview, setPreview] = useState<Preview>()
  const [error, setError] = useState<string>()
  const [loaded, setLoaded] = useState(false)
  const [attempt, setAttempt] = useState(0)
  const selected = artifacts.find((a) => s(a.artifactId) === artifactId)

  useEffect(() => {
    const rpc = session.rpc
    if (!rpc) {
      setError(L('実行環境に接続していません'))
      setLoaded(true)
      return
    }
    let alive = true
    void rpc.history(path(cardKey), true, 100).then((p) => {
      if (!alive) return
      setRuns(p.runs)
      setRunId((cur) => cur ?? (p.runs[0] ? s(p.runs[0].status!.runId) : undefined))
      setError(undefined)
    }).catch((e) => alive && setError(L('成果物を読み込めません: ') + errorText(e))).finally(() => alive && setLoaded(true))
    return () => {
      alive = false
    }
  }, [session.lastFinished, session.rpc, cardKey, attempt])
  useEffect(() => {
    const rpc = session.rpc
    if (!rpc || !runId) return setArtifacts([])
    let alive = true
    void rpc.artifacts(runId, path(cardKey)).then((a) => {
      if (!alive) return
      setArtifacts(a)
      setArtifactId(a[0] ? s(a[0].artifactId) : undefined)
      setMember(a[0]?.members[0] ? s(a[0].members[0].path) : undefined)
    }).catch(() => alive && setArtifacts([]))
    return () => {
      alive = false
    }
  }, [runId, cardKey])
  const read = (a: R.ArtifactInfoT, m: string) => session.rpc!.readArtifact(s(a.artifactId), a.kind === D.PortKind.Directory ? m : null)
  useEffect(() => {
    setPreview(undefined)
    if (!session.rpc || !selected || !member) return
    let url: string | undefined
    let alive = true
    void read(selected, member).then((data) => {
      if (!alive) return
      const ext = member.split('.').pop()?.toLowerCase() ?? ''
      const html = artifactHTML(member, data)
      if (html !== undefined) {
        setPreview({ kind: 'html', html })
      } else if (IMAGE_TYPES[ext]) {
        url = URL.createObjectURL(new Blob([data as BlobPart], { type: IMAGE_TYPES[ext] }))
        setPreview({ kind: 'image', url })
      } else {
        try {
          const t = new TextDecoder('utf-8', { fatal: true }).decode(data.subarray(0, 256 * 1024))
          setPreview({ kind: 'text', text: t + (data.length > 256 * 1024 ? L('\n…（先頭 256 KiB を表示）') : '') })
        } catch {
          setPreview({ kind: 'binary', size: data.length })
        }
      }
      setError(undefined)
    }).catch((e) => alive && setError(errorText(e)))
    return () => {
      alive = false
      if (url) URL.revokeObjectURL(url)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [artifactId, member])

  if (!loaded) return <div className="center-fill col gap8"><Spinner /><span className="secondary">{L('成果物を読み込んでいます…')}</span></div>
  if (!runs.length && error) {
    return (
      <Empty title={L('成果物を読み込めませんでした')} icon="exclamationmark.triangle" description={error}>
        <Button onClick={() => setAttempt(attempt + 1)}>{L('再試行')}</Button>
      </Empty>
    )
  }
  if (!runs.length) {
    const retains = bool(session.cardSetting(cardKey, 'retainArtifact')) === true
    return (
      <Empty title={L('保持された成果物はありません')} icon="shippingbox"
        description={retains ? L('成果物を保持する設定です。実行すると、ここに残ります。') : L('この Card は成果物を保持していません（設定 > 入出力 > 成果物を保持）。')} />
    )
  }

  const q = search.toLowerCase()
  const mark = runMark(runs.find(r => s(r.status?.runId) === runId))
  const members = (selected?.members ?? []).filter((m) => !q || s(m.path).toLowerCase().includes(q))
  return (
    <div className="col fill">
      <div className="row gap8 center pad6 border-bottom">
        <Select label={L('実行')} style={{ maxWidth: 360 }} value={runId ?? ''} onChange={(v) => setRunId(v || undefined)}
          options={runs.map((r) => ({ value: s(r.status!.runId), label: `${formatMillis(r.status!.acceptedAt)} · ${runState(r.status!.state).label}${runMark(r) ? ` · ${runMark(r)}` : ''}` }))} />
        <span className="spacer" />
        {mark && <span className="caption">{mark}</span>}
        {selected && member && (
          <Button onClick={() => void read(selected, member).then((d) => download(member.split('/').pop() ?? 'artifact', d)).catch((e) => setError(errorText(e)))}>{L('書き出す…')}</Button>
        )}
      </div>
      <div className="flex1 min0">
        <HSplit sizes={[220, 280, null]} min={[180, 220, 260]}>
          {[
            <div key="a" className="list fill" role="listbox" aria-label={L('成果物')}>
              {!artifacts.length && <Empty small title={L('成果物はありません')} icon="shippingbox" description={L('設定 > 入出力 で「成果物を保持」と保持する出力を選ぶと残ります。')} />}
              {artifacts.map((a) => (
                <div key={s(a.artifactId)} role="option" aria-selected={s(a.artifactId) === artifactId}
                  className={`list-row clickable col-start${s(a.artifactId) === artifactId ? ' selected' : ''}`}
                  onClick={() => { setArtifactId(s(a.artifactId)); setMember(a.members[0] ? s(a.members[0].path) : undefined) }}>
                  <b className="callout">{s(a.output)}</b>
                  <span className="caption secondary">#{String(a.invocation ?? 0n)} · {kindName(a.kind)} · {a.members.length} 件</span>
                </div>
              ))}
            </div>,
            <div key="m" className="col fill">
              <div className="pad6"><TextField small value={search} onChange={setSearch} placeholder={L('ファイルを検索')} style={{ width: '100%' }} /></div>
              <div className="list flex1" role="listbox" aria-label={L('ファイル')}>
                {members.map((m) => (
                  <div key={s(m.path)} role="option" aria-selected={s(m.path) === member} className={`list-row clickable${s(m.path) === member ? ' selected' : ''}`} onClick={() => setMember(s(m.path))}>
                    <Icon name="doc" /><span className="ellipsis flex1">{s(m.path)}</span>
                    <span className="caption secondary nowrap">{formatBytes(m.sizeBytes)}</span>
                  </div>
                ))}
              </div>
            </div>,
            <div key="p" className="fill scroll">
              {preview?.kind === 'html' && <ArtifactHTMLPreview html={preview.html} />}
              {preview?.kind === 'text' && <pre className="pad8 mono selectable small-text wrap-pre">{preview.text}</pre>}
              {preview?.kind === 'image' && (member?.endsWith('.pdf') ? <iframe title="preview" src={preview.url} className="fill" /> : <img src={preview.url} alt={member} className="preview-image" />)}
              {preview?.kind === 'binary' && <Empty title={L('プレビューできません')} icon="doc.questionmark" description={L('バイナリ {0} bytes。書き出して確認してください。', preview.size)} />}
              {!preview && <Empty title={member ? L('読み込み中…') : L('ファイルを選んでください')} icon="eye" />}
            </div>,
          ]}
        </HSplit>
      </div>
      {error && <div className="caption pad6 selectable" style={{ color: 'var(--red)' }}>{error}</div>}
    </div>
  )
}

/** A Card's output display enlarged in its own window. */
export function VisualizationWindow({ session, cardKey }: { session: ProjectSession; cardKey: CardKey }) {
  useObserve(session)
  useEffect(() => {
    void session.refreshVisibleDisplays()
  }, [session])
  const d = session.displays.get(cardKey)
  return (
    <div className="col fill pad16 gap8">
      <div className="row gap8 center">
        <span className="title2">{session.displayName(cardKey)}</span>
        {d && <span className="secondary">{L('呼び出し #{0}', String(d.invocation))}</span>}
        <span className="spacer" />
        <Button onClick={() => app.chat(session).addReference({ kind: 'display', card: cardKey })}>{L('チャットに参照を追加')}</Button>
      </div>
      <div className="flex1 min0 col"><DisplayContent display={d} large /></div>
    </div>
  )
}
