// Draft editor, the Card database and the project's settings panes.
import { ReactNode, useEffect, useState } from 'react'
import { app } from '../model/app'
import { useObserve } from '../model/observable'
import { FLOW_LABELS, FlowDirection } from '../model/prefs'
import { CardKey, definitionKind, errorText, formatBytes, key, KIND_ICON, KIND_LABEL, path, ProjectSession, TEMPLATES } from '../model/session'
import { arr, bool, get, isObj, J, JObj, obj, parse, serialize, str } from '../shared/json'
import { R, RPCFailure } from '../shared/rpc'
import { DEFINITION_DRAG } from './actions'
import { CardEditorModel, CardPreview, retargetWindows } from './cardSettings'
import { Button, Confirm, Empty, Form, HSplit, MenuButton, Row, Section, Select, Spinner, TextField, Toggle, Value } from './controls'
import { CodeEditor } from './editor'
import { Icon } from './icons'

// ---- Draft -------------------------------------------------------------------------

/** A planned port of a Draft: a name and what it is for (no type yet). */
interface DraftPort {
  id: number
  name: string
  purpose: string
}
let portIds = 0
const portsEq = (a: DraftPort[], b: DraftPort[]) => a.length === b.length && a.every((p, i) => p.name === b[i].name && p.purpose === b[i].purpose)

/** Shown in the empty content field: headings to write under. */
const DRAFT_TEMPLATE = `## 受け取るもの（入力）
- 例: 数値の列（仮の入力 numbers）

## 出すもの（出力）
- 例: 平均と最大値（仮の出力 summary）

## 処理の内容
- 例: 空の列ならエラーにする
- 例: 平均は小数第 2 位まで

## できたと言える条件
- 例: [1, 2, 3] から 平均 2、最大 3 が出る`

function draftPorts(m: CardEditorModel): { inputs: DraftPort[]; outputs: DraftPort[] } {
  const ports = (list: string, names: string[]) => {
    const notes = arr(get(m.meta, list)) ?? []
    const out = names.map((n) => ({ id: ++portIds, name: n, purpose: str(get(notes.find((x) => str(get(x, 'name')) === n), 'description')) ?? '' }))
    for (const n of notes) {
      const name = str(get(n, 'name'))
      if (name && !out.some((p) => p.name === name)) out.push({ id: ++portIds, name, purpose: str(get(n, 'description')) ?? '' })
    }
    return out
  }
  const names = (arr(get(m.definition, 'inputs')) ?? []).map((x) => str(get(x, 'name'))).filter((x): x is string => !!x)
  return { inputs: ports('inputs', names), outputs: ports('outputs', []) }
}

const uniquePort = (prefix: string, list: DraftPort[]) => {
  let n = 1
  while (list.some((p) => p.name === `${prefix}${n}`)) n++
  return `${prefix}${n}`
}

/** A Draft states what a Card will do before any code exists: its name, a
 *  summary and its content (what it takes, what it gives, how), with its
 *  planned ports. It can be wired and saved; "Card を生成" turns it into an
 *  implementation through the Agent. */
export function DraftEditor({ session, cardKey, onClose }: { session: ProjectSession; cardKey: CardKey; onClose: () => void }) {
  const [m, setModel] = useState(() => new CardEditorModel(session, cardKey))
  useObserve(m, session)
  const [name, setName] = useState(() => session.displayName(cardKey))
  const [initialPorts] = useState(() => draftPorts(m))
  const [inputs, setInputs] = useState<DraftPort[]>(initialPorts.inputs)
  const [outputs, setOutputs] = useState<DraftPort[]>(initialPorts.outputs)
  const [base, setBase] = useState(initialPorts)
  const token = m.followToken
  useEffect(() => m.follow(), [token, m])

  const load = (model: CardEditorModel) => {
    setName(session.displayName(model.cardKey))
    const p = draftPorts(model)
    setInputs(p.inputs)
    setOutputs(p.outputs)
    setBase(p)
  }
  // The window was pointed at another Card (renamed elsewhere): follow it.
  useEffect(() => {
    if (m.cardKey === cardKey) return
    const fresh = new CardEditorModel(session, cardKey)
    setModel(fresh)
    load(fresh)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [cardKey])

  if (!m.exists) return <Empty title="Draft が見つかりません" icon="questionmark.square.dashed" />
  if (m.kind !== 'draft') return <Empty title="Draft ではありません" icon="info.circle" description="実装済みの Card は Card 設定で編集します。" />

  const changed = !portsEq(inputs, base.inputs) || !portsEq(outputs, base.outputs)
  const dirty = m.unapplied || changed
  const metaField = (k: string) => ({
    value: str(get(m.meta, k)) ?? '',
    onChange: (v: string) => m.update((x) => {
      const meta: JObj = isObj(x.meta) ? x.meta : obj()
      meta.set(k, v || undefined)
      x.meta = meta
    }),
  })

  const apply = async (): Promise<boolean> => {
    // A port left without a name takes the one its placeholder suggests.
    const ins = inputs.map((p) => ({ ...p }))
    const outs = outputs.map((p) => ({ ...p }))
    for (const p of ins) if (!p.name) p.name = uniquePort('in', ins)
    for (const p of outs) if (!p.name) p.name = uniquePort('out', outs)
    setInputs(ins)
    setOutputs(outs)
    if (![...ins, ...outs].every((p) => /^[A-Za-z_][A-Za-z0-9_]*$/.test(p.name))) {
      m.update((x) => { x.message = 'ポート名は英字・数字・_ で、英字か _ で始めてください。' })
      return false
    }
    // Names in _card.json (inputs) and purposes in _meta.json (both sides).
    const notes = (ports: DraftPort[]): J[] | undefined => (ports.length ? ports.map((p) => {
      const o = obj([['name', p.name]])
      if (p.purpose) o.set('description', p.purpose)
      return o
    }) : undefined)
    m.update((x) => {
      if (isObj(x.definition)) x.definition.set('inputs', ins.length ? ins.map((p) => obj([['name', p.name]])) : undefined)
      const meta: JObj = isObj(x.meta) ? x.meta : obj()
      meta.set('inputs', notes(ins))
      meta.set('outputs', notes(outs))
      x.meta = meta
    })
    const ok = await m.apply()
    if (ok) load(m)
    return ok
  }

  const rename = async () => {
    const nw = name.trim()
    if (nw === session.displayName(m.cardKey)) return
    try {
      if (dirty && !(await apply())) return
      const from = m.cardKey
      await session.renameCard(from, nw)
      const to = key([...path(from).slice(0, -1), nw])
      retargetWindows(session, from, to)
      const fresh = new CardEditorModel(session, to)
      setModel(fresh)
      load(fresh)
    } catch (e) {
      m.update((x) => { x.message = e instanceof RPCFailure ? e.diagnostic ?? errorText(e) : errorText(e) })
      setName(session.displayName(m.cardKey))
    }
  }

  const close = async () => {
    if (dirty && !(await apply())) return
    onClose()
  }

  const portSection = (title: string, ports: DraftPort[], set: (p: DraftPort[]) => void, prefix: string, example: [string, string]) => (
    <Field title={title}>
      <div className="col gap6">
        {ports.map((p, i) => (
          <div key={p.id} className="row gap8 center">
            <TextField value={p.name} mono placeholder={i === 0 ? example[0] : `${prefix}${i + 1}`} style={{ width: 160 }}
              onChange={(v) => set(ports.map((x) => (x.id === p.id ? { ...x, name: v } : x)))} />
            <TextField value={p.purpose} placeholder={i === 0 ? example[1] : '用途の説明'} style={{ flex: 1 }}
              onChange={(v) => set(ports.map((x) => (x.id === p.id ? { ...x, purpose: v } : x)))} />
            <Button kind="borderless" iconOnly icon="minus.circle" help="この仮の入出力を削除" onClick={() => set(ports.filter((x) => x.id !== p.id))} />
          </div>
        ))}
        <div><Button kind="borderless" icon="plus.circle" onClick={() => set([...ports, { id: ++portIds, name: '', purpose: '' }])}>追加</Button></div>
      </div>
    </Field>
  )

  return (
    <div className="col fill" onKeyDown={(e) => {
      if (e.nativeEvent.isComposing) return
      if ((e.metaKey || e.ctrlKey) && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 's') {
        e.preventDefault()
        e.stopPropagation()
        if (dirty && session.isEditable) void apply()
      } else if (e.key === 'Escape') {
        e.preventDefault()
        e.stopPropagation()
        void close()
      }
    }}>
      <CardPreview session={session} model={m} height={130} />
      <div className="scroll flex1">
        <div className="col gap16 draft-form">
          <Field title="名前">
            <TextField value={name} onChange={setName} mono placeholder="英数字と - _ .（Card の ID とフォルダ名）"
              help="Return で名前を変えます（定義のフォルダ・接続・配置が新しい名前に移ります）"
              onKeyDown={(e) => { if (e.key === 'Enter' && !e.nativeEvent.isComposing) void rename() }} />
          </Field>
          <Field title="概要">
            <TextField {...metaField('description')} placeholder="例: 数値の列から平均と最大値を求める" />
          </Field>
          <Field title="内容">
            <div className="col gap4">
              <TextField {...metaField('content')} multiline rows={12} placeholder={DRAFT_TEMPLATE} style={{ minHeight: 230 }} />
              <span className="caption secondary">何を受け取り、何を出し、どう処理するかを自由に書きます。型やコードはまだ要りません。</span>
            </div>
          </Field>
          {portSection('仮の入力', inputs, setInputs, 'in', ['例: numbers', '例: 集計する数値の列'])}
          {portSection('仮の出力', outputs, setOutputs, 'out', ['例: summary', '例: 平均と最大値'])}
          <span className="caption secondary">仮の入出力で先に配線できます。適用した後に Canvas の「Card を生成」で実装を依頼します。</span>
        </div>
      </div>
      <div className="detail-foot">
        <span className="label secondary"><Icon name="pencil.and.outline" />未実装（実行されません）</span>
        {m.message && <span className="callout secondary clamp2 selectable">{m.message}</span>}
        <span className="spacer" />
        {dirty && <span className="label" style={{ color: 'var(--orange)' }}><Icon name="pencil.circle.fill" />未適用の変更</span>}
        <Button onClick={() => { m.reload(); load(m) }}>再読込</Button>
        <Button kind="prominent" disabled={!dirty || !session.isEditable} help="⌘S" onClick={() => void apply()}>適用</Button>
        <Button onClick={() => void close()}>閉じる</Button>
      </div>
    </div>
  )
}

function Field({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="col gap4">
      <span className="callout headline">{title}</span>
      {children}
    </div>
  )
}

// ---- Card database -----------------------------------------------------------------

/** Every definition folder of the project (one per Card, and those without
 *  a Card): search, edit its JSON, place an unplaced one on the canvas. */
export function CardDatabaseView({ session, definition }: { session: ProjectSession; definition?: string }) {
  useObserve(session)
  const [search, setSearch] = useState('')
  const [selected, setSelected] = useState<string | undefined>(() => (definition && session.definitions.has(definition) ? definition : undefined))
  const [cardText, setCardText] = useState('')
  const [metaText, setMetaText] = useState('')
  const [base, setBase] = useState({ card: '', meta: '' })
  const [parseError, setParseError] = useState<string>()
  const [message, setMessage] = useState<string>()

  const load = (id = selected) => {
    if (!id) return
    const c = session.definitions.get(id)
    const mt = session.definitionMeta.get(id)
    const ct = c !== undefined ? serialize(c) : '{}'
    const mtt = mt !== undefined ? serialize(mt) : ''
    setCardText(ct)
    setMetaText(mtt)
    setBase({ card: ct, meta: mtt })
    setParseError(undefined)
    setMessage(undefined)
  }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  useEffect(() => load(), [selected])
  useEffect(() => {
    if (definition && session.definitions.has(definition)) setSelected(definition)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [definition])

  const q = search.toLowerCase()
  const ids = [...session.definitions.keys()].sort().filter((id) =>
    !q || id.toLowerCase().includes(q) || (str(get(session.definitions.get(id), 'description')) ?? '').toLowerCase().includes(q))
  const unplaced = new Set(session.unplacedDefinitions)

  const apply = async (id: string) => {
    let card: J, meta: J | undefined
    try {
      card = parse(cardText)
      meta = metaText.trim() ? parse(metaText) : undefined
    } catch (e) {
      setParseError(`JSON として読めません: ${errorText(e)}`)
      return
    }
    setParseError(undefined)
    session.edit((x) => x.setEffectiveDefinition(id, { card, meta }))
    await session.save()
    setMessage(session.saveError ? `保存できませんでした: ${session.saveError}` : `適用しました（revision ${session.rev}）。`)
    load(id)
  }

  const detail = (() => {
    const id = selected
    if (!id || !session.definitions.has(id)) return <Empty key="none" title="定義を選んでください" icon="square.stack" description="ドラッグして Canvas に配置できます。" />
    const unapplied = cardText !== base.card || metaText !== base.meta
    const isUnplaced = unplaced.has(id)
    return (
      <div key="detail" className="col fill pad12 gap8">
        <div className="row center">
          <span className="title2 selectable">{id}</span>
          <span className="spacer" />
          {unapplied && <span className="label" style={{ color: 'var(--orange)' }}><Icon name="pencil.circle.fill" />未適用の変更</span>}
        </div>
        <div className="callout secondary">Card「{id}」の定義（definitions/{id}/）です。</div>
        <div className="headline">_card.json（設定・params・入力）</div>
        <CodeEditor text={cardText} onChange={setCardText} language="json" documentId={`db:${id}/_card.json`} style={{ flex: 2, minHeight: 180 }} />
        <div className="headline">_meta.json（説明・計画・追加コンテキスト、任意）</div>
        <CodeEditor text={metaText} onChange={setMetaText} language="json" documentId={`db:${id}/_meta.json`} style={{ flex: 1, minHeight: 80, maxHeight: 140 }} />
        {parseError && <span className="label selectable" style={{ color: 'var(--red)' }}><Icon name="xmark.octagon" />{parseError}</span>}
        {message && <div className="callout secondary selectable">{message}</div>}
        <div className="row gap8 center">
          <Button disabled={!session.isEditable || !isUnplaced} onClick={() => {
            const k = session.placeDefinition(id, { x: 40, y: 40 })
            setMessage(`${k} を Workflow に配置しました（未保存）。`)
          }}>Canvas に配置</Button>
          <Button disabled={isUnplaced} help="Card の設定画面で開きます"
            onClick={() => app.openWindow(session, { kind: 'card', card: key([id]), category: 'source' })}>ソースを開く…</Button>
          <Button kind="destructive" disabled={!isUnplaced || !session.isEditable}
            help={isUnplaced ? '定義とソースを削除' : 'Card を削除すると定義も削除されます'}
            onClick={() => { session.edit((x) => x.removeDefinition(id)); setSelected(undefined) }}>削除</Button>
          <span className="spacer" />
          <Button onClick={() => load()}>再読込</Button>
          <Button kind="prominent" disabled={!unapplied || !session.isEditable} onClick={() => void apply(id)}>適用</Button>
        </div>
      </div>
    )
  })()

  return (
    <HSplit sizes={[280, null]} min={[240, 420]}>
      {[
        <div key="list" className="col fill">
          <div className="row gap6 center pad6">
            <TextField value={search} onChange={setSearch} placeholder="検索" style={{ flex: 1 }} />
            <MenuButton icon="plus" help="Card を追加" disabled={!session.isEditable} items={() => TEMPLATES.map((t) => ({
              label: t.label,
              action: () => setSelected(path(session.addCard(t.id, [], { x: 40, y: 40 }))[0]),
            }))} />
          </div>
          <div className="list flex1" role="listbox" aria-label="定義">
            {ids.map((id) => {
              const kind = definitionKind(session.definitions.get(id))
              return (
                <div key={id} role="option" aria-selected={id === selected} className={`list-row clickable def-row${id === selected ? ' selected' : ''}`}
                  draggable onDragStart={(e) => { e.dataTransfer.setData('text/plain', DEFINITION_DRAG + id); e.dataTransfer.effectAllowed = 'copy' }}
                  onClick={() => setSelected(id)}>
                  <Icon name={KIND_ICON[kind]} className="secondary" />
                  <div className="col min0">
                    <span className="ellipsis">{id}</span>
                    <span className="caption secondary">{KIND_LABEL[kind] + (unplaced.has(id) ? ' · 未配置' : '')}</span>
                  </div>
                </div>
              )
            })}
          </div>
        </div>,
        detail,
      ]}
    </HSplit>
  )
}

// ---- Project settings --------------------------------------------------------------

export type ProjectSettingsPane = 'general' | 'databases' | 'history' | 'cache'
export const PROJECT_SETTINGS_PANES: { id: ProjectSettingsPane; title: string; icon: string }[] = [
  { id: 'general', title: '一般', icon: 'folder.badge.gearshape' },
  { id: 'databases', title: 'データベース接続先', icon: 'cylinder.split.1x2' },
  { id: 'history', title: '実行履歴', icon: 'clock.arrow.circlepath' },
  { id: 'cache', title: 'ビルドキャッシュ', icon: 'shippingbox' },
]

/** One of a project's settings panes (also shown in the settings window). */
export function ProjectSettingsContent({ pane, session }: { pane: ProjectSettingsPane; session: ProjectSession }) {
  useObserve(session)
  switch (pane) {
    case 'general': return <GeneralProjectSettings key={session.key} session={session} />
    case 'databases': return <DatabaseSettings key={session.key} session={session} />
    case 'history': return <RunHistorySettings key={session.key} session={session} />
    case 'cache': return <CacheSettings key={session.key} session={session} />
  }
}

/** The project's settings: its panes beside a sidebar. */
export function WorkflowSettingsView({ session, initial }: { session: ProjectSession; initial?: ProjectSettingsPane }) {
  const [pane, setPane] = useState<ProjectSettingsPane>(initial ?? 'general')
  return (
    <div className="split-view">
      <nav className="split-nav" aria-label="設定">
        {PROJECT_SETTINGS_PANES.map((p) => (
          <button key={p.id} type="button" className={`nav-row${p.id === pane ? ' selected' : ''}`} aria-current={p.id === pane} onClick={() => setPane(p.id)}>
            <Icon name={p.icon} />{p.title}
          </button>
        ))}
      </nav>
      <div className="split-detail"><ProjectSettingsContent pane={pane} session={session} /></div>
    </div>
  )
}

const setProject = (x: ProjectSession, k: string, v: J) => {
  if (!isObj(x.project)) x.project = obj()
  ;(x.project as JObj).set(k, v)
}

function GeneralProjectSettings({ session }: { session: ProjectSession }) {
  const [name, setName] = useState(str(get(session.project, 'name')) ?? '')
  const [pkgs, setPkgs] = useState(str(get(session.project, 'pkgsPath')) ?? '')
  const [message, setMessage] = useState<string>()
  const changed = name !== (str(get(session.project, 'name')) ?? '') || pkgs !== (str(get(session.project, 'pkgsPath')) ?? '')
  const save = async () => {
    session.edit((x) => {
      setProject(x, 'name', name)
      setProject(x, 'pkgsPath', pkgs || 'tsugumi:default')
    })
    await session.save()
    setMessage(session.saveError ?? '保存しました。')
  }
  return (
    <div className="col fill" onKeyDown={(e) => {
      // Return saves (the default action).
      if (e.key === 'Enter' && !e.nativeEvent.isComposing && changed && session.isEditable && (e.target as HTMLElement).tagName === 'INPUT') void save()
    }}>
      <div className="scroll flex1">
        <Form>
          <Section footer="「tsugumi:default」はアプリが固定した Nixpkgs です。変更すると Card の再ビルドが必要になります。">
            <Row label="プロジェクト名"><TextField value={name} onChange={setName} style={{ width: 260 }} /></Row>
            <Row label="実行環境（Nixpkgs）"><TextField value={pkgs} onChange={setPkgs} placeholder="tsugumi:default" style={{ width: 260 }} /></Row>
          </Section>
          <Section footer={<span className="label" style={{ color: 'var(--orange)' }}><Icon name="exclamationmark.triangle" />変えると Card の配置と線が合わなくなることがあります（「整列」で並べ直せます）。</span>}>
            <Row label="流れの向き" help="このプロジェクトの Card を並べた向きです（layout.json に保存され、すぐ反映されます）。線の引き方と「整列」がこの向きに合わせます。">
              <Select<FlowDirection> value={session.flow} onChange={(f) => session.setFlow(f)}
                options={(Object.keys(FLOW_LABELS) as FlowDirection[]).map((f) => ({ value: f, label: FLOW_LABELS[f] }))} />
            </Row>
          </Section>
          <Section>
            <Value label="場所" value={session.dir} />
          </Section>
        </Form>
      </div>
      <div className="detail-foot">
        {message && <span className="callout secondary selectable">{message}</span>}
        <span className="spacer" />
        <Button kind="prominent" disabled={!changed || !session.isEditable} onClick={() => void save()}>保存</Button>
      </div>
    </div>
  )
}

function DatabaseSettings({ session }: { session: ProjectSession }) {
  const read = () => (arr(get(session.project, 'databaseConnections')) ?? []).map((c, i) => ({ id: i, name: str(get(c, 'name')) ?? '', url: str(get(c, 'url')) ?? '' }))
  const [rows, setRows] = useState(read)
  const [base, setBase] = useState(rows)
  const [message, setMessage] = useState<string>()
  const changed = JSON.stringify(rows.map((r) => [r.name, r.url])) !== JSON.stringify(base.map((r) => [r.name, r.url]))
  const load = () => {
    const r = read()
    setRows(r)
    setBase(r)
    setMessage(undefined)
  }
  const save = async () => {
    session.edit((x) => setProject(x, 'databaseConnections', rows.map((r) => obj([['name', r.name], ['url', r.url]]))))
    await session.save()
    setMessage(session.saveError ?? '保存しました。')
    if (!session.saveError) load()
  }
  return (
    <div className="col fill">
      <div className="callout secondary pad12">記録 Card が外部 DB にログを保存するときの接続先です。core の履歴とは独立しています。</div>
      <div className="table flex1">
        <div className="table-head"><span>名前</span><span>URL</span><span /></div>
        {rows.map((r) => (
          <div key={r.id} className="table-row">
            <TextField small value={r.name} onChange={(v) => setRows(rows.map((x) => (x.id === r.id ? { ...x, name: v } : x)))} />
            <TextField small value={r.url} placeholder="postgresql://…" onChange={(v) => setRows(rows.map((x) => (x.id === r.id ? { ...x, url: v } : x)))} />
            <Button kind="borderless" iconOnly icon="minus.circle" help="削除" onClick={() => setRows(rows.filter((x) => x.id !== r.id))} />
          </div>
        ))}
      </div>
      <div className="detail-foot">
        <Button onClick={() => setRows([...rows, { id: Date.now(), name: `db${rows.length + 1}`, url: '' }])}>追加</Button>
        {message && <span className="callout secondary selectable">{message}</span>}
        <span className="spacer" />
        <Button onClick={load}>再読込</Button>
        <Button kind="prominent" disabled={!changed || !session.isEditable} onClick={() => void save()}>保存</Button>
      </div>
    </div>
  )
}

/** A date as the value of a datetime-local field (local time). */
const localInput = (d: Date) => {
  const p = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}`
}

function RunHistorySettings({ session }: { session: ProjectSession }) {
  const [scope, setScope] = useState<'before' | 'all'>('before')
  const [before, setBefore] = useState(() => localInput(new Date(Date.now() - 30 * 24 * 3600 * 1000)))
  const [working, setWorking] = useState(false)
  const [message, setMessage] = useState<string>()
  const [failed, setFailed] = useState(false)
  const [confirm, setConfirm] = useState(false)
  const saving = bool(get(session.project, 'saveRunHistory')) ?? true
  const setSaving = async (on: boolean) => {
    session.edit((x) => setProject(x, 'saveRunHistory', on))
    await session.save()
    setFailed(!!session.saveError)
    setMessage(session.saveError ? `保存できませんでした: ${session.saveError}` : undefined)
  }
  const del = async () => {
    setWorking(true)
    setMessage(undefined)
    try {
      const r = await session.deleteRunHistory(scope === 'all' ? null : new Date(before))
      setFailed(false)
      setMessage(`${r.deletedRuns ?? 0n} 件の実行を削除し、${formatBytes(r.freedBytes ?? 0n)} を解放しました。`)
    } catch (e) {
      setFailed(true)
      setMessage(`削除できませんでした: ${errorText(e)}`)
    }
    setWorking(false)
  }
  return (
    <div className="col fill">
      <div className="scroll flex1">
        <Form>
          <Section footer="実行履歴は VM の中に保存します。オンのときは、終わった実行の記録（値・型・ログ・成果物の一覧）をプロジェクトの .tsugumi/runs.sqlite にも書き出します。コピーしたり別の Mac に移したりしたプロジェクトは、開いたときに VM にない実行をここから読み込みます。オフにすると、新しい実行は VM の中だけに保存します。">
            <Row label="実行履歴をプロジェクトに保存する">
              <Toggle checked={saving} disabled={!session.isEditable} onChange={(v) => void setSaving(v)} />
            </Row>
          </Section>
          <Section title="削除" footer="実行の記録・値・ログと、実行ごとのファイル（成果物など、.tsugumi/runs/<run>）を、VM 内と .tsugumi/runs.sqlite の両方から削除します。最後の入力（その Card だけを再実行するための入力）は残します。実行中は削除できません。">
            <Row label="対象">
              <Select<'before' | 'all'> value={scope} onChange={setScope}
                options={[{ value: 'before', label: '指定した日時より前に終わった実行' }, { value: 'all', label: 'すべての実行' }]} />
            </Row>
            {scope === 'before' && (
              <Row label="日時">
                <input type="datetime-local" className="field" value={before} onChange={(e) => setBefore(e.target.value)} />
              </Row>
            )}
          </Section>
        </Form>
      </div>
      <div className="detail-foot">
        {message && <span className="callout selectable" style={{ color: failed ? 'var(--red)' : 'var(--secondary)' }}>{message}</span>}
        <span className="spacer" />
        {working && <Spinner small />}
        <Button kind="destructive" disabled={working || session.runActive || !session.connected || (scope === 'before' && !before)} onClick={() => setConfirm(true)}>削除…</Button>
      </div>
      {confirm && <Confirm title={scope === 'all' ? 'すべての実行履歴を削除しますか？' : 'この日時より前に終わった実行を削除しますか？'} confirm="削除"
        onConfirm={() => void del()} onCancel={() => setConfirm(false)} />}
    </div>
  )
}

function CacheSettings({ session }: { session: ProjectSession }) {
  const [result, setResult] = useState<R.CacheClearResultT>()
  const [working, setWorking] = useState(false)
  const [message, setMessage] = useState<string>()
  const [confirm, setConfirm] = useState(false)
  const clear = async () => {
    setWorking(true)
    setMessage(undefined)
    try {
      setResult(await session.clearBuildCache())
    } catch (e) {
      setMessage(`削除できませんでした: ${errorText(e)}`)
    }
    setWorking(false)
  }
  return (
    <div className="col fill pad16 gap12">
      <div>このプロジェクトの Card のビルド結果の登録（Nix の GC root）と、作業用のビルドファイルを削除します。共有 Nix store にある実体は消さず、アプリ設定の「共有 store を掃除」で登録のないものとして削除されます。文書・実行履歴・保持した成果物は削除しません。次回の実行では評価し直し、store に残っていればビルドせずに使います。</div>
      <div className="callout secondary">実行中・SDK 準備中は削除できません。</div>
      {result && (
        <div className="groupbox col gap4">
          <div className="headline">結果</div>
          <Value label="削除" value={`${result.deleted.length} 件`} />
          <Value label="保持" value={`${result.retained.length} 件`} />
          {result.freedBytes !== null && <Value label="解放" value={formatBytes(result.freedBytes)} />}
        </div>
      )}
      {message && <div className="selectable" style={{ color: 'var(--red)' }}>{message}</div>}
      <span className="flex1" />
      <div className="row end gap8">
        {working && <Spinner small />}
        <Button kind="destructive" disabled={working || session.runActive || !session.connected} onClick={() => setConfirm(true)}>削除</Button>
      </div>
      {confirm && <Confirm title="ビルドキャッシュを削除しますか？" confirm="削除" onConfirm={() => void clear()} onCancel={() => setConfirm(false)} />}
    </div>
  )
}
