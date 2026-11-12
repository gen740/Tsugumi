// Run history, artifacts and the enlarged output display of one Card.
import { useEffect, useMemo, useState } from 'react'
import { app } from '../model/app'
import { artifactHTML } from '../model/artifactHTML'
import { useObserve } from '../model/observable'
import { CardKey, cardState, errorText, formatBytes, formatDuration, formatMillis, path, ProjectSession, runState } from '../model/session'
import { bool } from '../shared/json'
import { describeValue } from '../shared/flatjson'
import { D, R, s, segs } from '../shared/rpc'
import { Button, download, Empty, Form, HSplit, Row, Section, Segmented, Select, Sheet, Spinner, TextField, Value } from './controls'
import { RunComparisonView } from './runComparison'
import { ArtifactHTMLPreview } from './artifactHTMLPreview'
import { DisplayContent } from './display'
import { InvocationPackages } from './environments'
import { Icon } from './icons'
import { RunLogView } from './logs'
import { L } from '../i18n'

type Filter = 'all' | 'succeeded' | 'failed'
type Detail = 'io' | 'log' | 'packages' | 'info'

/** Runs of one Card (filter by success/failure) → invocations → I/O, log, info.
 *  The packages are those recorded for the invocation.
 *  Loading, failed and empty are said plainly, never as an empty split view. */
export function CardExecutionView({ session, cardKey }: { session: ProjectSession; cardKey: CardKey }) {
  useObserve(session)
  const [runs, setRuns] = useState<R.RunSummaryT[]>([])
  const [filter, setFilter] = useState<Filter>('all')
  const [runId, setRunId] = useState<string>()
  const [invocations, setInvocations] = useState<R.InvocationSummaryT[]>([])
  const [invocation, setInvocation] = useState<bigint>()
  const [record, setRecord] = useState<R.RecordResultT>()
  const [detail, setDetail] = useState<Detail>('io')
  const [error, setError] = useState<string>()
  const [loaded, setLoaded] = useState(false)
  const [attempt, setAttempt] = useState(0)
  const [comparing, setComparing] = useState(false)

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
  }, [session.lastFinished, session.rpc, cardKey, attempt])
  // Another run: its own latest call is shown, never the previous run's number.
  useEffect(() => {
    setInvocation(undefined)
    setRecord(undefined)
    const rpc = session.rpc
    if (!rpc || !runId) return setInvocations([])
    let alive = true
    void rpc.invocations(runId, path(cardKey)).then((p) => {
      if (!alive) return
      setInvocations(p.records)
      setInvocation(p.records[p.records.length - 1]?.invocation ?? undefined)
    }).catch(() => alive && setInvocations([]))
    return () => {
      alive = false
    }
  }, [runId, cardKey])
  useEffect(() => {
    const rpc = session.rpc
    if (!rpc || !runId) return setRecord(undefined)
    let alive = true
    void rpc.record(runId, path(cardKey), invocation ?? null, invocation === undefined)
      .then((r) => alive && setRecord(r)).catch(() => alive && setRecord(undefined))
    return () => {
      alive = false
    }
  }, [runId, invocation, cardKey])

  if (!loaded) return <div className="center-fill col gap8"><Spinner /><span className="secondary">{L('履歴を読み込んでいます…')}</span></div>
  if (!runs.length && error) {
    return (
      <Empty title={L('履歴を読み込めませんでした')} icon="exclamationmark.triangle" description={error}>
        <Button onClick={() => setAttempt(attempt + 1)}>{L('再試行')}</Button>
      </Empty>
    )
  }
  if (!runs.length) return <Empty title={L('まだ実行していません')} icon="clock" description={L('この Card が実行されると、ここに実行ごとの入出力・ログ・情報が残ります。')} />

  const filtered = runs.filter((r) => {
    const st = r.status!.state
    return filter === 'all' || (filter === 'succeeded' ? st === D.RunState.succeeded : st === D.RunState.failed || st === D.RunState.cancelled)
  })
  const rec = record?.record
  return (
    <HSplit sizes={[260, null]} min={[220, 380]}>
      {[
        <div key="runs" className="col fill">
          <div className="pad6"><Button disabled={runs.length < 2} onClick={() => setComparing(true)}>{L('実行を比較')}</Button></div>
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
                    <div className="caption secondary ellipsis">{rs.label} · 対象 {segs(r.target).join('/')}</div>
                  </div>
                </div>
              )
            })}
          </div>
        </div>,
        <div key="detail" className="col fill">
          {!runId ? <Empty title={L('実行を選んでください')} icon="list.bullet.rectangle" /> : (
            <>
              <div className="row gap8 center pad6 border-bottom">
                <Select label={L('呼び出し')} style={{ maxWidth: 320 }} value={invocation === undefined ? '' : String(invocation)}
                  onChange={(v) => setInvocation(v ? BigInt(v) : undefined)}
                  options={invocations.length ? invocations.map((i) => ({
                    value: String(i.invocation ?? 0n),
                    label: `#${i.invocation ?? 0n} · ${cardState(i.state).label}` + (i.durationMs !== null ? ` · ${formatDuration(Number(i.durationMs) / 1000)}` : ''),
                  })) : [{ value: '', label: L('呼び出しなし') }]} />
                <span className="spacer" />
                <Segmented<Detail> small width={300} value={detail} onChange={setDetail} options={[{ value: 'io', label: L('入出力') }, { value: 'log', label: L('ログ') }, { value: 'packages', label: L('依存 package') }, { value: 'info', label: L('情報') }]} />
              </div>
              <div className="flex1 col min0">
                {detail === 'io' && (rec ? (
                  <div className="scroll pad10 col gap8">
                    {(rec.recordIo === false || record?.recordingDisabled) && <span className="label secondary"><Icon name="info.circle" />{L('この Card は入出力を記録していません（設定 > 入出力 > 入出力を記録）。')}</span>}
                    <div className="headline">{L('入力')}</div>
                    {!rec.inputs.length && <span className="secondary">{L('なし')}</span>}
                    {rec.inputs.map((v, i) => <ValueBox key={i} v={v} />)}
                    <div className="headline">{L('出力')}</div>
                    {!rec.outputs.length && <span className="secondary">{L('なし')}</span>}
                    {rec.outputs.map((v, i) => <ValueBox key={i} v={v} />)}
                  </div>
                ) : record?.exists === false ? <Empty title={L('この実行では呼び出されていません')} icon="circle.dotted" /> : <Empty title={L('記録がありません')} icon="tray" />)}
                {detail === 'log' && <RunLogView session={session} runId={runId} card={path(cardKey)} invocation={invocation} />}
                {/* What this invocation ran with, from its record; never the current definition. */}
                {detail === 'packages' && (record?.exists === false ? <Empty title={L('この実行では呼び出されていません')} icon="circle.dotted" />
                  : <InvocationPackages session={session} runId={runId} environmentId={s(rec?.summary?.environmentId)} />)}
                {detail === 'info' && (record ? (
                  <div className="scroll">
                    <Form>
                      <Section>
                        <Value label={L('状態')} value={cardState(record.status!.state).label} />
                        {rec?.summary && (
                          <>
                            <Value label={L('呼び出し')} value={`#${rec.summary.invocation ?? 0n}`} />
                            <Value label={L('段階')} value={D.Phase[rec.summary.phase] ?? ''} />
                            <Value label={L('開始')} value={formatMillis(rec.summary.startedAt)} />
                            <Value label={L('終了')} value={formatMillis(rec.summary.endedAt)} />
                            {rec.summary.durationMs !== null && <Value label={L('所要時間')} value={formatDuration(Number(rec.summary.durationMs) / 1000)} />}
                            {rec.summary.executionService && <Value label={L('実行サービス')} value={s(rec.summary.executionService) === 'local' ? L('プロジェクトの実行環境') : s(rec.summary.executionService)} />}
                            {/* A Card built for another system (its `system` setting): what ran it. */}
                            {rec.summary.emulator && <Value label={L('エミュレーター')} value={s(rec.summary.emulator)} />}
                            {rec.summary.waitingInputs.length > 0 && <Value label={L('待っていた入力')} value={rec.summary.waitingInputs.map((w) => s(w.port)).join(', ')} />}
                          </>
                        )}
                        {record.status!.failure && (
                          <>
                            <Value label={L('失敗の種類')} value={D.FailureKind[record.status!.failure.kind] ?? String(record.status!.failure.kind)} />
                            <Row><span className="selectable wrap-pre" style={{ color: 'var(--red)' }}>{s(record.status!.failure.diagnostic)}</span></Row>
                          </>
                        )}
                        <Value label={L('集計')} value={L('呼び出し {0} · 成功 {1} · 失敗 {2}', record.aggregate?.calls ?? 0n, record.aggregate?.succeeded ?? 0n, record.aggregate?.failed ?? 0n)} />
                      </Section>
                      {rec && rec.inputSources.length > 0 && (
                        <Section title={L('入力元')}>
                          {rec.inputSources.map((x, i) => <Value key={i} label={s(x.port)} value={`${segs(x.card).join('/')} #${x.invocation ?? 0n}`} />)}
                        </Section>
                      )}
                      {record.orphaned && <div className="secondary pad10">{L('この Card は文書から削除されています。')}</div>}
                    </Form>
                  </div>
                ) : <Empty title={L('情報がありません')} icon="info.circle" />)}
              </div>
            </>
          )}
          {error && <div className="caption pad6" style={{ color: 'var(--red)' }}>{error}</div>}
        </div>,
      ]}
    </HSplit>
  )
}

/** A recorded input/output value as pretty JSON (decoded with its own BFBS). */
function ValueBox({ v }: { v: D.NamedValueT }) {
  const text = useMemo(() => describeValue({
    name: s(v.name), rootType: s(v.rootType), sizeBytes: v.sizeBytes, artifactId: v.artifactId === null ? null : s(v.artifactId),
    inlineBytes: v.inlineBytes, schemaBfbs: v.schemaBfbs,
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
  const members = (selected?.members ?? []).filter((m) => !q || s(m.path).toLowerCase().includes(q))
  return (
    <div className="col fill">
      <div className="row gap8 center pad6 border-bottom">
        <Select label={L('実行')} style={{ maxWidth: 360 }} value={runId ?? ''} onChange={(v) => setRunId(v || undefined)}
          options={runs.map((r) => ({ value: s(r.status!.runId), label: `${formatMillis(r.status!.acceptedAt)} · ${runState(r.status!.state).label}` }))} />
        <span className="spacer" />
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
