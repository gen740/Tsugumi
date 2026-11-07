// The Card database and the project's settings panes.
import { useEffect, useState } from 'react'
import { app } from '../model/app'
import { shortCommit, sourceName, unlockedRef } from '../model/nixSources'
import { useObserve } from '../model/observable'
import { FLOW_LABELS, FlowDirection } from '../model/prefs'
import { definitionKind, errorText, formatBytes, key, KIND_ICON, KIND_LABEL, path, ProjectSession, TEMPLATES } from '../model/session'
import { arr, bool, get, isObj, J, JObj, obj, parse, serialize, str } from '../shared/json'
import { R } from '../shared/rpc'
import { DEFINITION_DRAG } from './actions'
import { Button, Confirm, Empty, Form, HSplit, MenuButton, Row, Section, Select, Spinner, TextField, Toggle, Value } from './controls'
import { CodeEditor } from './editor'
import { Icon } from './icons'

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
    !q || id.toLowerCase().includes(q) || (str(get(session.definitionMeta.get(id), 'description')) ?? '').toLowerCase().includes(q))
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

export type ProjectSettingsPane = 'general' | 'canvas' | 'databases' | 'history' | 'cache'
export const PROJECT_SETTINGS_PANES: { id: ProjectSettingsPane; title: string; icon: string }[] = [
  { id: 'general', title: '名前と実行環境', icon: 'folder.badge.gearshape' },
  { id: 'canvas', title: 'Canvas', icon: 'rectangle.3.group' },
  { id: 'databases', title: 'データベース接続先', icon: 'cylinder.split.1x2' },
  { id: 'history', title: '実行履歴', icon: 'clock.arrow.circlepath' },
  { id: 'cache', title: 'ビルドキャッシュ', icon: 'shippingbox' },
]

/** One of a project's settings panes (also shown in the settings window). */
export function ProjectSettingsContent({ pane, session }: { pane: ProjectSettingsPane; session: ProjectSession }) {
  useObserve(session)
  switch (pane) {
    case 'general': return <GeneralProjectSettings key={session.key} session={session} />
    case 'canvas': return <CanvasProjectSettings session={session} />
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

/** Flakes this project opts in to: an address gives its commit, hash and packages, which Cards read as inputs. */
function NixSourceSettings({ session }: { session: ProjectSession }) {
  useObserve(session)
  const sources = arr(get(session.project, 'nixSources')) ?? []
  const [address, setAddress] = useState('')
  const [found, setFound] = useState<{ name: string; ref: string; packages: string[] }>()
  const [working, setWorking] = useState(false)
  const [message, setMessage] = useState<string>()
  const resolve = async (ref: string) => {
    setWorking(true)
    setMessage(undefined)
    try {
      const r = await session.resolveNixSource(ref)
      return { ref: String(r.ref), packages: r.packages.map(String) }
    } catch (e) {
      setMessage(`取得できませんでした: ${errorText(e)}`)
      return undefined
    } finally {
      setWorking(false)
    }
  }
  const save = async (list: J[]) => {
    session.edit((x) => setProject(x, 'nixSources', list))
    await session.save()
    setMessage(session.saveError ? `保存できませんでした: ${session.saveError}` : undefined)
  }
  const entry = (name: string, ref: string) => obj([['name', name], ['type', 'flake'], ['ref', ref]])
  const look = async () => {
    const r = await resolve(address.trim())
    if (r) setFound({ name: sourceName(address.trim()), ...r })
  }
  const add = async () => {
    if (!found) return
    await save([...sources, entry(found.name, found.ref)])
    setFound(undefined)
    setAddress('')
  }
  const update = async (name: string, ref: string) => {
    const r = await resolve(unlockedRef(ref))
    if (r) await save(sources.map((s) => (str(get(s, 'name')) === name ? entry(name, r.ref) : s)))
  }
  const taken = found !== undefined && sources.some((s) => str(get(s, 'name')) === found.name)
  return (
    <Section title="外部ソース" footer="ここに追加した flake の package を、Card の default.nix から inputs.sources.<名前>.packages.<package> で使えます。コミットと hash で固定します。flake の式は Card の隔離の外で評価するので、信頼できるものだけを追加してください。">
      {sources.map((s) => {
        const name = str(get(s, 'name')) ?? ''
        const ref = str(get(s, 'ref')) ?? ''
        return (
          <Row key={name} label={name}>
            <code className="secondary" title={ref}>{unlockedRef(ref)} {shortCommit(ref)}</code>
            <Button disabled={working || !session.isEditable} onClick={() => void update(name, ref)}>更新</Button>
            <Button kind="destructive" disabled={working || !session.isEditable} onClick={() => void save(sources.filter((x) => x !== s))}>削除</Button>
          </Row>
        )
      })}
      <Row label="flake">
        <TextField value={address} onChange={(v) => { setAddress(v); setFound(undefined) }} placeholder="github:owner/repo" mono style={{ width: 260 }} disabled={working || !session.isEditable} />
        <Button disabled={working || !address.trim() || !session.isEditable} onClick={() => void look()}>取得</Button>
        {working && <Spinner small />}
      </Row>
      {found && (
        <>
          <Row label="名前"><TextField value={found.name} onChange={(name) => setFound({ ...found, name })} mono style={{ width: 160 }} /></Row>
          <Row label="コミット"><code className="secondary" title={found.ref}>{shortCommit(found.ref) ?? found.ref}</code></Row>
          <Row label="package"><code className="secondary">{found.packages.join(', ') || 'この環境の package はありません'}</code></Row>
          <Row><Button disabled={taken || !/^[A-Za-z_][A-Za-z0-9_-]*$/.test(found.name)} onClick={() => void add()}>追加</Button>{taken && <span className="secondary caption">同じ名前があります</span>}</Row>
        </>
      )}
      {message && <Row><span className="label caption" style={{ color: 'var(--orange)', whiteSpace: 'normal' }}>{message}</span></Row>}
    </Section>
  )
}

function GeneralProjectSettings({ session }: { session: ProjectSession }) {
  const [name, setName] = useState(str(get(session.project, 'name')) ?? '')
  const [pkgs, setPkgs] = useState(str(get(session.project, 'pkgsPath')) ?? '')
  const [message, setMessage] = useState<string>()
  const pin = app.status?.nixpkgsPin ?? undefined
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
          <Section footer="このプロジェクトが使う Nixpkgs です（版の一部として残ります）。変更すると Card の再ビルドが必要になります。">
            <Row label="プロジェクト名"><TextField value={name} onChange={setName} style={{ width: 260 }} /></Row>
            <Row label="実行環境（Nixpkgs）"><TextField value={pkgs} onChange={setPkgs} placeholder={pin ?? 'tsugumi:default'} style={{ width: 260 }} /></Row>
            {(!pkgs || pkgs === 'tsugumi:default') && (
              <Row><span className="label caption" style={{ color: 'var(--orange)', whiteSpace: 'normal' }}><Icon name="exclamationmark.triangle" />Nixpkgs が固定されていません。Tsugumi を更新すると、このプロジェクトの Card のパッケージも変わります。</span></Row>
            )}
            {pin && pkgs !== pin && (
              <Row><Button onClick={() => setPkgs(pin)}>{!pkgs || pkgs === 'tsugumi:default' ? 'この Tsugumi の Nixpkgs に固定' : 'この Tsugumi の Nixpkgs に更新'}</Button></Row>
            )}
          </Section>
          <NixSourceSettings session={session} />
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

/** How the project's Cards are laid out (layout.json, applied at once). */
function CanvasProjectSettings({ session }: { session: ProjectSession }) {
  return (
    <div className="scroll fill">
      <Form>
        <Section footer={<span className="label" style={{ color: 'var(--orange)' }}><Icon name="exclamationmark.triangle" />変えると Card の配置と線が合わなくなることがあります（「整列」で並べ直せます）。</span>}>
          <Row label="流れの向き" help="このプロジェクトの Card を並べた向きです（layout.json に保存され、すぐ反映されます）。線の引き方と「整列」がこの向きに合わせます。">
            <Select<FlowDirection> value={session.flow} onChange={(f) => session.setFlow(f)}
              options={(Object.keys(FLOW_LABELS) as FlowDirection[]).map((f) => ({ value: f, label: FLOW_LABELS[f] }))} />
          </Row>
        </Section>
      </Form>
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
  const [result, setResult] = useState<R.ProjectCacheClearResultT>()
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
