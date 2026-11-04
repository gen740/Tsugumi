// Card settings: a local working copy of one Card's settings (its
// `_card.json`), metadata and sources. Nothing reaches the project until
// "適用". Every Card has its own definition folder, so what is edited here is
// this Card only (inside a Card in Card: the Card of the external, this
// project's copy).
import { ReactNode, useEffect, useLayoutEffect, useRef, useState } from 'react'
import { api } from '../api'
import { app } from '../model/app'
import { cloneDef, merge3 } from '../model/merge'
import { Observable, useObserve } from '../model/observable'
import { OutputCatalogResult, parseOutputCatalog } from '../model/outputCatalog'
import { prefs } from '../model/prefs'
import { CardKey, errorText, key, KIND_ICON, KIND_LABEL, paramText, parseParam, path, PortInfo, ProjectSession } from '../model/session'
import { CardEditorCategory, CATEGORY } from '../model/workspace'
import { BFBS } from '../shared/bfbs'
import { arr, bool, clone, get, isObj, J, jeq, JNum, JObj, numText, obj, serialize, str } from '../shared/json'
import { D, RPCFailure, s, sOpt } from '../shared/rpc'
import { CardNodeView } from './cardNode'
import { Button, Confirm, Empty, Form, HSplit, Prompt, Row, Section, Select, showMenu, Spinner, TextField, Toggle, Value } from './controls'
import { CodeEditor, EditorMarker } from './editor'
import { CardArtifactsView, CardExecutionView } from './history'
import { Icon } from './icons'
import { FolderPicker } from './sidebar'
import { SymbolPicker } from './symbolPicker'

/** Whether a source file's content is known. A file that could not be read
 *  is never shown or treated as an empty file. */
export type SourceLoad = { state: 'loading' } | { state: 'loaded' } | { state: 'failed'; error: string }

/** Local working copy of one Card. It follows what changes elsewhere while it
 *  is open (the Agent, the canvas): settings and files not touched here take
 *  the new content; those changed on both sides are listed as conflicts, and
 *  applying keeps what was edited here. */
export class CardEditorModel extends Observable {
  definition?: J
  meta?: J
  /** Loaded or edited source text. */
  texts = new Map<string, string>()
  removed = new Set<string>()
  created = new Set<string>()
  message?: string
  applying = false
  /** Settings and files changed both here and elsewhere since the copy was taken. */
  conflicts: string[] = []
  /** Category shown in the window (kept with the model, so edits and the place survive switching). */
  category: CardEditorCategory = 'general'
  loads = new Map<string, SourceLoad>()
  /** Bumped by every reload, so open editors read their file again if needed. */
  generation = 0
  /** Files open as tabs and the one shown (kept across categories and applies). */
  openFiles: string[] = []
  shownFile?: string
  collapsedFolders = new Set<string>()
  private baseDefinition?: J
  private baseMeta?: J
  private baseTexts = new Map<string, string>()
  /** Source hash when each file was read. */
  private baseHashes = new Map<string, string>()

  constructor(readonly session: ProjectSession, readonly cardKey: CardKey) {
    super()
    this.reload()
  }

  /** The Card's definition folder name (its id). */
  get definitionId(): string | undefined {
    return path(this.cardKey).pop()
  }
  get kind() {
    return this.session.kind(this.cardKey)
  }
  get exists() {
    return this.session.instance(this.cardKey) !== undefined
  }
  private get prefix(): string {
    return this.session.definitionDir(this.cardKey) ?? ''
  }

  get unapplied(): boolean {
    return !jeq(this.definition, this.baseDefinition) || !jeq(this.meta, this.baseMeta) || this.removed.size > 0 || this.created.size > 0 ||
      [...this.texts].some(([k, v]) => this.baseTexts.get(k) !== v)
  }

  /** Source files of the definition (documents excluded), with local changes. */
  get files(): string[] {
    const prefix = this.prefix
    if (!prefix) return []
    const docs = new Set([prefix + '_card.json', prefix + '_meta.json', prefix + '_link.json'])
    const all = new Set([...this.session.sourcePaths.filter((p) => p.startsWith(prefix) && !docs.has(p)), ...this.created])
    for (const p of this.removed) all.delete(p)
    return [...all].sort()
  }

  /** Changes whenever what this copy follows changes (the settings, the files' hashes). */
  get followToken(): string {
    const prefix = this.prefix
    const files = this.session.sourcePaths.filter((p) => p.startsWith(prefix)).map((p) => `${p}=${this.session.sources.get(p)?.hash ?? '-'}`)
    const ser = (v: J | undefined) => (v === undefined ? '' : serialize(v, false))
    return [ser(this.session.cardDefinition(this.cardKey)), ser(this.session.cardMeta(this.cardKey))].join('\u0001') + files.join(';')
  }

  /** Takes the project's current state as the new base. After an apply
   *  (`keepingSources`) the files stay open with the text just saved, which
   *  is now their base; otherwise they are read again. */
  reload(keepingSources = false) {
    const keep = keepingSources ? [...this.texts].filter(([p]) => !this.removed.has(p)) : []
    this.definition = clone(this.session.cardDefinition(this.cardKey))
    this.meta = clone(this.session.cardMeta(this.cardKey))
    this.baseDefinition = clone(this.definition)
    this.baseMeta = clone(this.meta)
    this.texts = new Map()
    this.baseTexts = new Map()
    this.baseHashes = new Map()
    this.loads = new Map()
    for (const [p, t] of keep) {
      this.texts.set(p, t)
      this.baseTexts.set(p, t)
      this.loads.set(p, { state: 'loaded' })
      const h = this.session.sources.get(p)?.hash
      if (h) this.baseHashes.set(p, h)
    }
    this.generation++
    this.removed = new Set()
    this.created = new Set()
    this.message = undefined
    this.conflicts = []
    this.changed()
  }

  /** Takes in what changed elsewhere since the copy was taken. */
  follow() {
    const defNow = this.session.cardDefinition(this.cardKey), metaNow = this.session.cardMeta(this.cardKey)
    const clashes = (base: J | undefined, ours: J | undefined, theirs: J | undefined, file: string): string[] => {
      if (jeq(ours, base) || jeq(theirs, base) || jeq(ours, theirs)) return []
      const ks = (v: J | undefined) => (isObj(v) ? v.keys() : [])
      const keys = [...new Set([...ks(base), ...ks(ours), ...ks(theirs)])].sort()
      return keys.filter((k) => !jeq(get(base, k), get(ours, k)) && !jeq(get(base, k), get(theirs, k)) && !jeq(get(ours, k), get(theirs, k)))
        .map((k) => `${file}: ${k}`)
    }
    const found: string[] = []
    let any = false
    if (!jeq(defNow, this.baseDefinition)) {
      found.push(...clashes(this.baseDefinition, this.definition, defNow, '_card.json'))
      this.definition = clone(merge3(this.baseDefinition, this.definition, defNow))
      this.baseDefinition = clone(defNow)
      any = true
    }
    if (!jeq(metaNow, this.baseMeta)) {
      found.push(...clashes(this.baseMeta, this.meta, metaNow, '_meta.json'))
      this.meta = clone(merge3(this.baseMeta, this.meta, metaNow))
      this.baseMeta = clone(metaNow)
      any = true
    }
    for (const p of [...this.texts.keys()]) {
      if (this.loads.get(p)?.state !== 'loaded' || this.created.has(p)) continue
      const h = this.session.sources.get(p)?.hash
      const base = this.baseHashes.get(p)
      if (base === undefined || h === base) continue
      if (this.isEdited(p)) {
        found.push(p.slice(this.prefix.length))
      } else {
        // Not edited here: read the new content.
        this.texts.delete(p)
        this.baseTexts.delete(p)
        this.baseHashes.delete(p)
        this.loads.delete(p)
        this.generation++
        any = true
      }
    }
    if (found.length) this.conflicts = [...new Set([...this.conflicts, ...found])].sort()
    if (any || found.length) this.changed()
  }

  openFile(p: string) {
    if (!this.openFiles.includes(p)) this.openFiles = [...this.openFiles, p]
    this.shownFile = p
    this.changed()
  }

  closeFile(p: string) {
    const i = this.openFiles.indexOf(p)
    if (i < 0) return
    this.openFiles = this.openFiles.filter((f) => f !== p)
    if (this.shownFile === p) this.shownFile = this.openFiles.length ? this.openFiles[Math.min(i, this.openFiles.length - 1)] : undefined
    this.changed()
  }

  /** Reads a source file once (later calls return the working copy).
   *  Throws when it cannot be read; nothing is recorded as its content then. */
  async text(p: string): Promise<string> {
    const have = this.texts.get(p)
    if (have !== undefined) return have
    this.loads.set(p, { state: 'loading' })
    this.changed()
    const hash = this.session.sources.get(p)?.hash
    try {
      const t = await this.session.sourceText(p)
      if (!this.texts.has(p)) {
        this.texts.set(p, t)
        this.baseTexts.set(p, t)
        if (hash) this.baseHashes.set(p, hash)
      }
      this.loads.set(p, { state: 'loaded' })
      this.changed()
      return this.texts.get(p) ?? t
    } catch (e) {
      this.loads.set(p, { state: 'failed', error: errorText(e) })
      this.changed()
      throw e
    }
  }

  /** The working copy of a file, only once it was read (or created here). */
  loadedText(p: string): string | undefined {
    return this.texts.get(p)
  }

  setText(p: string, t: string) {
    this.texts.set(p, t)
    this.changed()
  }

  /** The file was changed here and not applied yet. */
  isEdited(p: string): boolean {
    return this.texts.has(p) && (this.created.has(p) || this.texts.get(p) !== this.baseTexts.get(p))
  }

  /** `_outputs.fbs` of the definition. */
  get outputsPath(): string | undefined {
    return this.prefix ? this.prefix + '_outputs.fbs' : undefined
  }

  /** Output ports from the edited, unapplied `_outputs.fbs` (undefined: not
   *  edited, the resolved ports apply). */
  get editedOutputs(): OutputCatalogResult | undefined {
    const p = this.outputsPath
    if (!p) return undefined
    if (this.removed.has(p)) return { ports: [] }
    const t = this.texts.get(p)
    if (!this.isEdited(p) || t === undefined) return undefined
    return parseOutputCatalog(t)
  }

  createFile(rel: string, text = '') {
    const prefix = this.prefix
    if (!prefix || !rel) return
    const p = prefix + rel
    this.removed.delete(p)
    this.created.add(p)
    this.texts.set(p, text)
    this.loads.set(p, { state: 'loaded' })
    this.changed()
  }

  /** Renames by copying the content that was actually read; a file that
   *  cannot be read is left as it is. */
  async rename(p: string, rel: string): Promise<boolean> {
    let t: string
    try {
      t = await this.text(p)
    } catch (e) {
      this.message = '読み込めないため名前を変更しませんでした: ' + errorText(e)
      this.changed()
      return false
    }
    this.removeFile(p)
    this.createFile(rel, t)
    return true
  }

  removeFile(p: string) {
    if (this.created.has(p)) this.created.delete(p)
    else this.removed.add(p)
    this.texts.delete(p)
    this.loads.delete(p)
    this.changed()
  }

  /** Mutates the working copy (definition, meta). */
  update(fn: (m: this) => void) {
    fn(this)
    this.changed()
  }

  /** Sets a setting of the working copy's `_card.json` (undefined removes it). */
  setSetting(name: string, v: J | undefined) {
    if (!isObj(this.definition)) return
    this.definition.set(name, v)
    this.changed()
  }

  /** Copies the working copy into the project as one undoable edit (saved at once). */
  async apply(): Promise<boolean> {
    const session = this.session
    if (!session.isEditable) {
      this.message = '実行中は Card を変更できません。実行が終わってから適用してください。'
      this.changed()
      return false
    }
    this.follow()
    this.applying = true
    this.changed()
    const def = this.definition, m = this.meta, k = this.cardKey, prefix = this.prefix
    const changedTexts = [...this.texts].filter(([p, v]) => this.baseTexts.get(p) !== v || this.created.has(p))
    const gone = [...this.removed]
    const bases = new Map(this.baseHashes)
    const clashing = new Set(this.conflicts)
    session.edit((x) => {
      const d = x.cardDefinitionDraft(k)
      if (def !== undefined && d && d.link === undefined && (!jeq(def, d.card) || !jeq(m, d.meta))) {
        x.setCardDefinition(k, { ...cloneDef(d), card: clone(def), meta: clone(m) })
      }
      for (const [p, t] of changedTexts) {
        x.putSource(p, t)
        // Saved against the version this editor read (a file changed on both
        // sides replaces the other version: the user chose so by applying).
        const h = bases.get(p)
        if (h && !clashing.has(p.slice(prefix.length))) x.sourceBases.set(p, h)
      }
      for (const p of gone) {
        x.removeSource(p)
        const h = bases.get(p)
        if (h) x.sourceBases.set(p, h)
      }
    })
    await session.save()
    this.applying = false
    if (session.saveError) {
      this.message = '保存できませんでした: ' + session.saveError
      this.changed()
      return false
    }
    this.reload(true)
    this.message = '適用しました。'
    this.changed()
    return true
  }
}

/** Windows showing a renamed Card (or a Card inside it) follow it. */
export function retargetWindows(session: ProjectSession, from: CardKey, to: CardKey) {
  let moved = false
  for (const w of app.windows) {
    if (w.session !== session || !('card' in w.what)) continue
    const c = w.what.card
    if (c === from || c.startsWith(from + '/')) {
      w.what = { ...w.what, card: to + c.slice(from.length) }
      moved = true
    }
  }
  if (moved) app.changed()
}

function categories(m: CardEditorModel): CardEditorCategory[] {
  switch (m.kind) {
    case 'code': return ['general', 'nix', 'source', 'ports', 'executions', 'artifacts', 'context']
    case 'input': return ['general', 'source', 'ports', 'executions', 'artifacts', 'context']
    case 'draft': return ['general', 'ports', 'context']
    case 'subgraph': return ['general', 'ports', 'executions']
    default: return ['general', 'ports', 'executions', 'artifacts', 'context']
  }
}

export function CardSettingsEditor({ session, card, initial, onClose }: { session: ProjectSession; card: CardKey; initial?: CardEditorCategory; onClose: () => void }) {
  const [model, setModel] = useState(() => {
    const m = new CardEditorModel(session, card)
    m.category = initial ?? 'general'
    return m
  })
  useObserve(model, session)
  const [confirmReload, setConfirmReload] = useState(false)
  // The window was pointed at another Card (renamed elsewhere): follow it.
  useEffect(() => {
    if (model.cardKey === card) return
    const m = new CardEditorModel(session, card)
    m.category = model.category
    setModel(m)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [card])
  // What changes elsewhere while the window is open is taken in at once.
  const token = model.followToken
  useEffect(() => model.follow(), [token, model])

  if (!model.exists) {
    return <Empty title="Card が見つかりません" icon="questionmark.square.dashed" description={`${model.cardKey} は削除されたか、まだ保存されていません。`} />
  }
  const renamed = (to: CardKey) => {
    const m = new CardEditorModel(session, to)
    m.category = model.category
    retargetWindows(session, model.cardKey, to)
    setModel(m)
  }
  const close = async () => {
    if (model.unapplied && !(await model.apply())) return
    onClose()
  }
  const cats = categories(model)
  const category = cats.includes(model.category) ? model.category : 'general'
  return (
    <div className="cse" onKeyDown={(e) => {
      if (e.nativeEvent.isComposing) return
      // ⌘S applies; Esc applies and closes (as the buttons' shortcuts).
      if ((e.metaKey || e.ctrlKey) && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 's') {
        e.preventDefault()
        e.stopPropagation()
        if (model.unapplied && !model.applying && session.isEditable) void model.apply()
      } else if (e.key === 'Escape') {
        e.preventDefault()
        e.stopPropagation()
        void close()
      }
    }}>
      <nav className="cse-sidebar" aria-label="カテゴリ">
        {cats.map((c) => (
          <button key={c} type="button" className={`nav-row${c === category ? ' selected' : ''}`} aria-current={c === category}
            onClick={() => model.update((m) => { m.category = c })}>
            <Icon name={CATEGORY[c].icon} />{CATEGORY[c].title}
          </button>
        ))}
      </nav>
      <div className="cse-main">
        {/* The preview belongs to 一般; editors, ports and history use the whole height. */}
        {category === 'general' && <CardPreview session={session} model={model} height={170} />}
        <div className="cse-content">
          {category === 'general' && <GeneralCategory model={model} onRenamed={renamed} />}
          {category === 'nix' && <SourceCategory model={model} only="default.nix" />}
          {category === 'source' && <SourceCategory model={model} />}
          {category === 'ports' && (model.kind === 'subgraph' ? <BoundaryCategory model={model} /> : <PortsCategory model={model} />)}
          {category === 'executions' && <CardExecutionView session={session} cardKey={model.cardKey} />}
          {category === 'artifacts' && <CardArtifactsView session={session} cardKey={model.cardKey} />}
          {category === 'context' && <ContextCategory model={model} />}
        </div>
        {model.conflicts.length > 0 && (
          <div className="cse-conflicts">
            <Icon name="exclamationmark.triangle.fill" style={{ color: 'var(--orange)' }} />
            <span className="callout flex1">別の場所でも変更されました: {model.conflicts.join(', ')}。適用するとここでの編集が残ります。</span>
            <Button onClick={() => setConfirmReload(true)}>最新を読み込む</Button>
          </div>
        )}
        <div className="cse-footer">
          {model.unapplied && <span className="label" style={{ color: 'var(--orange)' }}><Icon name="pencil.circle.fill" />未適用の変更</span>}
          {model.message ? <span className="callout clamp2 selectable" style={{ color: session.saveError ? 'var(--red)' : 'var(--secondary)' }}>{model.message}</span>
            : !session.isEditable ? <span className="label secondary"><Icon name="lock.fill" />実行中は変更を適用できません</span> : null}
          <span className="spacer" />
          {model.applying && <Spinner small />}
          <Button onClick={() => (model.unapplied ? setConfirmReload(true) : model.reload())}>再読込</Button>
          <Button kind="prominent" disabled={!model.unapplied || model.applying || !session.isEditable} help="⌘S" onClick={() => void model.apply()}>適用</Button>
          <Button help="未適用の変更は適用してから閉じます" onClick={() => void close()}>閉じる</Button>
        </div>
      </div>
      {confirmReload && <Confirm title="未適用の変更を破棄して読み込み直しますか？" confirm="破棄して再読込" onConfirm={() => model.reload()} onCancel={() => setConfirmReload(false)} />}
    </div>
  )
}

// ---- Preview -----------------------------------------------------------------------

/** The Card as the canvas draws it, from the working copy (unapplied changes
 *  included). It can be selected and collapsed; nothing here changes the
 *  project. */
export function CardPreview({ session, model, height }: { session: ProjectSession; model: CardEditorModel; height: number }) {
  useObserve(model, session, prefs)
  const [collapsed, setCollapsed] = useState(() => session.collapsed(model.cardKey))
  const [selected, setSelected] = useState(false)
  const box = useRef<HTMLDivElement>(null)
  const inner = useRef<HTMLDivElement>(null)
  const [scale, setScale] = useState(1)
  const edited = model.editedOutputs
  const outputs = edited === undefined ? undefined : 'ports' in edited ? edited.ports : []
  const data = session.nodeData(model.cardKey, { definition: model.definition, meta: model.meta, collapsed, outputs })
  // Fits the Card into the preview (never larger than on the canvas at 100%).
  useLayoutEffect(() => {
    const b = box.current, i = inner.current
    if (!b || !i) return
    const fit = () => {
      const w = i.offsetWidth, h = i.offsetHeight
      if (!w || !h) return
      setScale(Math.min(1, (b.clientWidth - 32) / w, (b.clientHeight - 32) / h))
    }
    fit()
    const ro = new ResizeObserver(fit)
    ro.observe(b)
    ro.observe(i)
    return () => ro.disconnect()
  }, [])
  let notice: ReactNode = null
  if (edited && 'error' in edited) {
    notice = <span className="cse-notice" style={{ color: 'var(--orange)' }}><Icon name="exclamationmark.triangle.fill" />_outputs.fbs を読めません: {edited.error}（出力ポートは表示していません）</span>
  } else if (edited) {
    notice = <span className="cse-notice secondary"><Icon name="pencil.circle" />編集中の _outputs.fbs（未適用・型は適用後に解決）</span>
  } else if (session.saving) {
    notice = <span className="cse-notice secondary"><Icon name="hourglass" />解決中…</span>
  }
  return (
    <div ref={box} className={`cse-preview${prefs.showGrid ? ' grid' : ''}`} style={{ height, ['--grid' as string]: `${prefs.gridSize}px` }}
      onPointerDown={(e) => { if (e.target === e.currentTarget) setSelected(false) }}>
      <div ref={inner} className="cse-preview-card" style={{ transform: `scale(${scale})` }}
        onPointerDown={() => setSelected(true)}>
        <CardNodeView data={data} flow={session.flow} selected={selected} onToggleCollapse={() => setCollapsed(!collapsed)} />
      </div>
      {notice && <div className="cse-preview-notice">{notice}</div>}
      <span className="cse-preview-label caption2 secondary">プレビュー</span>
    </div>
  )
}

// ---- Categories --------------------------------------------------------------------

/** Name (the Card's id and folder), description, params, when it runs, and
 *  the worker's permissions. */
function GeneralCategory({ model, onRenamed }: { model: CardEditorModel; onRenamed: (to: CardKey) => void }) {
  const session = model.session
  const def = model.definition
  const [name, setName] = useState(() => session.displayName(model.cardKey))
  const [renaming, setRenaming] = useState(false)
  const [renameError, setRenameError] = useState<string>()
  // This tsugumid runs on this host: without the sandbox a Card is a process
  // of the user's, so turning it off is the user's knowing choice.
  const [confirmSandboxOff, setConfirmSandboxOff] = useState(false)
  useEffect(() => setName(session.displayName(model.cardKey)), [model.cardKey])

  const rename = async () => {
    const nw = name.trim()
    if (nw === session.displayName(model.cardKey)) return
    setRenaming(true)
    setRenameError(undefined)
    try {
      if (model.unapplied && !(await model.apply())) {
        setRenaming(false)
        return
      }
      await session.renameCard(model.cardKey, nw)
      // The editor follows the Card under its new name.
      onRenamed(key([...path(model.cardKey).slice(0, -1), nw]))
    } catch (e) {
      setRenameError(e instanceof RPCFailure ? e.diagnostic ?? errorText(e) : errorText(e))
      setName(session.displayName(model.cardKey))
    }
    setRenaming(false)
  }
  const string = (n: string, fallback?: string) => ({
    value: str(get(def, n)) ?? fallback ?? '',
    onChange: (v: string) => model.setSetting(n, !v || v === fallback ? undefined : v),
  })
  const toggle = (n: string, label: string, fallback: boolean, help?: string) => (
    <Row label={label} help={help}>
      <Toggle checked={bool(get(def, n)) ?? fallback} onChange={(v) => model.setSetting(n, v === fallback ? undefined : v)} />
    </Row>
  )
  const numberField = (n: string, label: string, fallback: string) => (
    <Row label={label}>
      <TextField value={numText(get(def, n)) ?? str(get(def, n)) ?? ''} placeholder={fallback} style={{ width: 140 }}
        onChange={(v) => {
          const t = v.trim()
          // The core reports non-numbers.
          model.setSetting(n, !t ? undefined : /^[0-9]+$/.test(t) ? new JNum(t) : t)
        }} />
    </Row>
  )
  const timeoutField = (n: string, label: string) => (
    <Row label={label}>
      <TextField value={get(def, n) === null ? '' : numText(get(def, n)) ?? ''} placeholder="無期限" style={{ width: 140 }}
        onChange={(v) => model.setSetting(n, v.trim() ? new JNum(v.trim()) : undefined)} />
    </Row>
  )
  const params = arr(get(def, 'params')) ?? []
  const diags = session.resolution.get(model.cardKey)?.diagnostics ?? []
  const kind = model.kind
  return (
    <Form>
      <Section>
        <Row label="名前" help="名前は Card の ID で、定義のフォルダ名（definitions/<名前>/）です。変えると接続・配置・実行履歴も追従します（すぐに保存します）。">
          <div className="row gap6 center">
            <TextField value={name} onChange={setName} mono style={{ width: 220 }}
              onKeyDown={(e) => { if (e.key === 'Enter' && !e.nativeEvent.isComposing) void rename() }} />
            {name !== session.displayName(model.cardKey) && (
              <Button disabled={renaming || !session.isEditable} onClick={() => void rename()}>名前を変更</Button>
            )}
            {renaming && <Spinner small />}
          </div>
        </Row>
        {renameError && <Row><span className="caption selectable" style={{ color: 'var(--red)' }}>{renameError}</span></Row>}
        <Value label="種類" value={KIND_LABEL[kind]} />
        {def !== undefined && (
          <Row label="アイコン">
            <SymbolPicker symbol={str(get(def, 'symbol')) || undefined} inherited={KIND_ICON[kind]}
              onChange={(v) => model.setSetting('symbol', v)} />
          </Row>
        )}
        <Row label="概要">
          <TextField {...string('description')} multiline rows={2} style={{ width: '100%', minHeight: 38, maxHeight: 90 }} />
        </Row>
      </Section>
      {params.length > 0 && (
        <Section title="パラメータ">
          {params.map((_, i) => <ParamRow key={i} model={model} index={i} />)}
        </Section>
      )}
      {kind !== 'subgraph' && kind !== 'draft' && (
        <Section title="実行">
          {kind === 'code' && (
            <Row label="trigger" help="allInputs: 必須の入力がすべて届いたら 1 回動きます。eachInput: 値が 1 つ届くたびに動きます。">
              <Select {...string('trigger', 'allInputs')} options={[
                { value: 'allInputs', label: 'すべての入力が揃ったとき' }, { value: 'eachInput', label: '入力が届くたび' },
              ]} />
            </Row>
          )}
          {numberField('maxRuns', '最大実行回数', '1')}
          {timeoutField('timeoutSeconds', '実行の期限（秒）')}
          {kind === 'logger' && toggle('markdown', 'Markdown として表示', false,
            'オン: 受け取った文字列（文字列のフィールドが 1 つのメッセージ）を Markdown と数式（$…$、$$…$$）で表示します。ほかのメッセージは JSON をコードとして表示します。')}
          {kind === 'code' && (
            <>
              {timeoutField('buildTimeoutSeconds', 'ビルドの期限（秒）')}
              {toggle('reuseWorker', 'worker の状態を残す', true,
                'オン: 読み込んだ worker を次の呼び出しや次の実行でも使い、状態（static 変数など）を残します。オフ: 呼び出しごとに新しい worker です。')}
              {toggle('filesystem', '作業・出力領域を使う', false)}
              <Row label="ファイルアクセスを制限">
                <Toggle checked={bool(get(def, 'sandbox')) ?? true}
                  onChange={(v) => (v ? model.setSetting('sandbox', undefined) : setConfirmSandboxOff(true))} />
              </Row>
              {confirmSandboxOff && (
                <Confirm title="この Card を隔離なしで実行しますか？" confirm="隔離なしで実行する"
                  message="Web UI の tsugumid はこのマシンで直接動いています。隔離をやめると、この Card はあなたの権限でこのマシンのファイル・ネットワーク・プロセスに触れられます。Linux で他人の Card を動かすときは、tsugumid を隔離されたコンテナの中で動かしてください。"
                  onConfirm={() => { setConfirmSandboxOff(false); model.setSetting('sandbox', false) }} onCancel={() => setConfirmSandboxOff(false)} />
              )}
            </>
          )}
        </Section>
      )}
      {kind === 'code' && (
        <Section title={<span className="label"><Icon name="flask" />Experimental</span>}>
          {toggle('network', 'ネットワークへのアクセスを許可する', false, 'オフ（デフォルト）: preRun・run・postRun のどれからも外へ通信できません。')}
          <Row><HostFoldersEditor model={model} /></Row>
        </Section>
      )}
      {diags.length > 0 && (
        <Section title="診断">
          {diags.map((d) => (
            <Row key={d.id}>
              <span className="label selectable" style={{ color: d.error ? 'var(--red)' : undefined }}>
                <Icon name={d.error ? 'xmark.circle' : 'exclamationmark.triangle'} />{d.message}
              </span>
            </Row>
          ))}
        </Section>
      )}
    </Form>
  )
}

/** Folders of the host this Card's worker may read (Experimental): each at
 *  /host/<name> in the worker, read-only unless allowed to write. Only the
 *  folder's path is kept in the project. */
function HostFoldersEditor({ model }: { model: CardEditorModel }) {
  const folders = arr(get(model.definition, 'hostFolders')) ?? []
  const [picking, setPicking] = useState(false)
  const [present, setPresent] = useState<Record<string, boolean>>({})
  const paths = folders.map((f) => str(get(f, 'path')) ?? '')
  const pathsKey = paths.join('\n')
  useEffect(() => {
    let alive = true
    for (const p of paths) {
      if (!p || p in present) continue
      void api(`/api/fs?path=${encodeURIComponent(p)}`).then(() => true, () => false).then((ok) => {
        if (alive) setPresent((x) => ({ ...x, [p]: ok }))
      })
    }
    return () => {
      alive = false
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [pathsKey])
  const edit = (fn: (list: J[]) => void) => {
    const list = folders.map((f) => clone(f))
    fn(list)
    model.setSetting('hostFolders', list.length ? list : undefined)
  }
  const add = (p: string) => {
    const taken = new Set(folders.map((f) => str(get(f, 'name'))).filter(Boolean))
    const last = p.replace(/\/+$/, '').split('/').pop() ?? ''
    const base = [...last].map((c) => (/[A-Za-z0-9\-_.]/.test(c) ? c : '-')).join('')
    let name = !base || base === '.' || base === '..' ? 'folder' : base
    for (let n = 2; taken.has(name); n++) name = `${base}-${n}`
    edit((l) => l.push(obj([['path', p], ['name', name]])))
  }
  return (
    <div className="col gap6 fill">
      <div className="row center">
        <span>ホストのフォルダへのアクセスを許可する</span>
        <span className="spacer" />
        <Button onClick={() => setPicking(true)}>フォルダを追加…</Button>
      </div>
      {folders.map((f, i) => {
        const p = str(get(f, 'path')) ?? ''
        const ok = present[p] !== false
        return (
          <div key={i} className="row gap6 center host-folder">
            <Icon name={ok ? 'folder' : 'folder.badge.questionmark'} style={{ color: ok ? 'var(--secondary)' : 'var(--orange)' }} />
            <div className="col flex1 min0">
              <span className="callout ellipsis" title={p}>{p}</span>
              <span className="caption" style={{ color: ok ? 'var(--secondary)' : 'var(--orange)' }}>
                {ok ? `/host/${str(get(f, 'name')) ?? ''}` : 'このホストにはありません（実行すると失敗します）'}
              </span>
            </div>
            <Toggle kind="checkbox" label="書き込み" help="オフ（デフォルト）: 読み取り専用" checked={bool(get(f, 'write')) ?? false}
              onChange={(v) => edit((l) => isObj(l[i]) && (l[i] as JObj).set('write', v ? true : undefined))} />
            <Button kind="borderless" iconOnly icon="minus.circle" help="削除" onClick={() => edit((l) => l.splice(i, 1))} />
          </div>
        )
      })}
      <div className="caption secondary">選んだフォルダだけを、この Card の worker から /host/&lt;名前&gt; として読めます。プロジェクトに保存するのはフォルダの場所だけです。追加・変更はプロジェクトを開き直すと反映されます。</div>
      {picking && <FolderPicker message="worker から読めるようにするフォルダを選んでください" onClose={() => setPicking(false)} onPick={(p) => { setPicking(false); add(p) }} />}
    </div>
  )
}

function ParamRow({ model, index }: { model: CardEditorModel; index: number }) {
  const param = (arr(get(model.definition, 'params')) ?? [])[index] ?? obj()
  const name = str(get(param, 'name')) ?? '?'
  const type = str(get(param, 'type')) ?? 'string'
  const value = get(param, 'data')
  const set = (v: J | undefined) => {
    const params = (arr(get(model.definition, 'params')) ?? []).map((p) => clone(p))
    if (index >= params.length || !isObj(params[index])) return
    ;(params[index] as JObj).set('data', v)
    model.setSetting('params', params)
  }
  return (
    <Row label={name}>
      <div className="row gap6 center">
        {type === 'bool'
          ? <Toggle checked={bool(value) ?? false} onChange={(v) => set(v)} />
          : <TextField value={paramText(value)} onChange={(t) => set(parseParam(t, type))} style={{ width: 220 }} />}
        <span className="caption2 secondary nowrap">{type + (bool(get(param, 'nix')) ? ' · nix' : '')}</span>
      </div>
    </Row>
  )
}

/** A port's purpose in _meta.json. */
function portNote(model: CardEditorModel, list: string, name: string): { value: string; onChange: (v: string) => void } {
  return {
    value: str(get((arr(get(model.meta, list)) ?? []).find((x) => str(get(x, 'name')) === name), 'description')) ?? '',
    onChange: (v) => model.update((m) => {
      const meta: JObj = isObj(m.meta) ? m.meta : obj()
      const items = (arr(meta.get(list)) ?? []).map((x) => clone(x))
      const i = items.findIndex((x) => str(get(x, 'name')) === name)
      if (i >= 0 && isObj(items[i])) (items[i] as JObj).set('description', v || undefined)
      else if (v) items.push(obj([['name', name], ['description', v]]))
      meta.set(list, items.length ? items : undefined)
      m.meta = meta.size ? meta : undefined
    }),
  }
}

/** Ports, their purposes, recording and artifact retention. */
function PortsCategory({ model }: { model: CardEditorModel }) {
  const r = model.session.resolution.get(model.cardKey)
  const def = model.definition
  const editable = def !== undefined && ['code', 'draft', 'merge'].includes(model.kind)
  const inputs = arr(get(def, 'inputs')) ?? []
  const editInputs = (fn: (a: J[]) => void) => {
    const a = inputs.map((x) => clone(x))
    fn(a)
    model.setSetting('inputs', a.length ? a : undefined)
  }
  const keepable = (r?.outputs ?? []).filter((p) => p.kind === D.PortKind.File || p.kind === D.PortKind.Directory)
  return (
    <Form>
      <Section title="入力ポート" footer="入力の型は接続元の出力から決まります（_inputs.fbs は生成され、手で書きません）。">
        {editable ? (
          <>
            {inputs.map((inp, i) => {
              const name = str(get(inp, 'name')) ?? ''
              return (
                <Row key={i}>
                  <div className="row gap8 center fill">
                    <TextField value={name} placeholder="名前" mono help="入力ポートの名前" style={{ width: 140 }}
                      onChange={(v) => editInputs((a) => isObj(a[i]) && (a[i] as JObj).set('name', v))} />
                    <TextField {...portNote(model, 'inputs', name)} placeholder="用途（何を受け取るか）" style={{ flex: 1 }}
                      help="このポートの用途（_meta.json の説明）。Agent とほかの人への説明です。" />
                    <span className="caption secondary nowrap">{r?.inputs.find((p) => p.name === name)?.rootType ?? '未解決'}</span>
                    <Button kind="borderless" iconOnly icon="minus.circle" help="入力を削除" onClick={() => editInputs((a) => a.splice(i, 1))} />
                  </div>
                </Row>
              )
            })}
            <Row><Button onClick={() => editInputs((a) => a.push(obj([['name', `in${a.length + 1}`]])))}>入力を追加</Button></Row>
          </>
        ) : (
          <>
            {(r?.inputs ?? []).map((p) => <Value key={p.name} label={p.name} value={p.rootType ?? '未解決'} />)}
            {!(r?.inputs ?? []).length && <Row><span className="secondary">入力はありません</span></Row>}
          </>
        )}
      </Section>
      <OutputPortsSection model={model} />
      {model.kind !== 'subgraph' && model.kind !== 'draft' && (
        <Section title="記録と成果物">
          <Row label="入出力を記録">
            <Toggle checked={bool(get(def, 'recordIO')) ?? false} onChange={(v) => model.setSetting('recordIO', v ? true : undefined)} />
          </Row>
          <Row label="成果物を保持">
            <Toggle checked={bool(get(def, 'retainArtifact')) ?? false} onChange={(v) => model.setSetting('retainArtifact', v ? true : undefined)} />
          </Row>
          {/* Only File / Directory outputs can be kept as artifacts. */}
          <Row label="保持する出力">
            {keepable.length === 0
              ? <span className="callout secondary">{r === undefined ? '出力の型がまだ解決されていません' : 'File / Directory の出力がないため、保持できる出力はありません'}</span>
              : <Select value={str(get(def, 'artifactOutput')) ?? ''} onChange={(v) => model.setSetting('artifactOutput', v || undefined)}
                  options={[{ value: '', label: '選んでください' },
                    ...keepable.map((p) => ({ value: p.name, label: `${p.name}（${p.kind === D.PortKind.File ? 'File' : 'Directory'}）` }))]} />}
          </Row>
        </Section>
      )}
    </Form>
  )
}

const shortType = (full: string) => full.split('.').pop() ?? full

/** Output ports as `_outputs.fbs` defines them (its source is the truth).
 *  Each port's type is chosen from the tables of that schema, as the core
 *  compiles it (bfbs; the app never parses fbs), and written back to it. */
function OutputPortsSection({ model }: { model: CardEditorModel }) {
  const session = model.session
  const r = session.resolution.get(model.cardKey)
  const [types, setTypes] = useState<string[]>([])
  const [error, setError] = useState<string>()
  const p = model.outputsPath
  const loaded = p ? model.loadedText(p) : undefined
  useEffect(() => {
    let alive = true
    const rpc = session.rpc
    if (!p || !rpc || (!session.sources.has(p) && loaded === undefined)) {
      setTypes([])
      return
    }
    void (async () => {
      try {
        const text = await model.text(p)
        const res = await rpc.outputTypes(text)
        const schema = new BFBS(Uint8Array.from(res.bfbs))
        const root = s(res.rootType)
        if (!alive) return
        setTypes(schema.objects.filter((o) => !o.isStruct && o.name !== root).map((o) => o.name).sort())
        setError(sOpt(res.error))
      } catch {
        if (alive) setTypes([])
      }
    })()
    return () => {
      alive = false
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [p, model.generation, loaded, session.rpc])

  /** Rewrites the port in the working copy of _outputs.fbs (applied with the rest). */
  const set = async (port: string, type: string, kind: D.PortKind) => {
    const rpc = session.rpc
    if (!type || !p || !rpc) return
    try {
      const text = await model.text(p)
      const res = await rpc.outputTypes(text, port, type, kind)
      const changed = sOpt(res.text)
      if (changed !== undefined) {
        model.setText(p, changed)
        setError(undefined)
      } else {
        setError(sOpt(res.error) ?? '変更できませんでした')
      }
    } catch (e) {
      setError(errorText(e))
    }
  }
  const ports: PortInfo[] = r?.outputs ?? []
  return (
    <Section title="出力ポート" footer="出力ポートと型の正本は _outputs.fbs です（型はここで選べます。型そのものはソースで定義します）。">
      {ports.map((port) => (
        <Row key={port.name}>
          <div className="row gap8 center fill">
            <span className="mono ellipsis" style={{ width: 120, flex: 'none' }}>{port.name}</span>
            <TextField {...portNote(model, 'outputs', port.name)} placeholder="用途（何を出すか）" style={{ flex: 1 }} />
            {types.length === 0 || !p ? (
              <span className="caption secondary nowrap">{(port.rootType ?? '未解決') + (port.optional ? '（省略可）' : '')}</span>
            ) : (
              <>
                <Select value={port.rootType ?? ''} style={{ maxWidth: 150 }} help="_outputs.fbs で定義した型から選びます（_outputs.fbs に書き戻します）"
                  onChange={(t) => void set(port.name, t, port.kind)}
                  options={[...(port.rootType === undefined ? [{ value: '', label: '未解決' }] : []),
                    ...(port.rootType !== undefined && !types.includes(port.rootType) ? [{ value: port.rootType, label: shortType(port.rootType) }] : []),
                    ...types.map((t) => ({ value: t, label: shortType(t) }))]} />
                <Select<number> value={port.kind} style={{ maxWidth: 100 }} help="File / Directory は、型に文字列の value（パス）がある場合に選べます"
                  onChange={(k) => void set(port.name, port.rootType ?? '', k)}
                  options={[{ value: D.PortKind.FlatBuffer, label: '値' }, { value: D.PortKind.File, label: 'File' }, { value: D.PortKind.Directory, label: 'Directory' },
                    ...(port.kind === D.PortKind.unspecified ? [{ value: D.PortKind.unspecified, label: '—' }] : [])]} />
              </>
            )}
          </div>
        </Row>
      ))}
      {ports.length === 0 && <Row><span className="secondary">出力はありません</span></Row>}
      {error && <Row><span className="caption selectable" style={{ color: 'var(--red)' }}>{error}</span></Row>}
    </Section>
  )
}

/** Inputs and outputs of a Card in Card: its external project's boundary
 *  (each outer port maps to a port of a Card inside). */
function BoundaryCategory({ model }: { model: CardEditorModel }) {
  const b = model.session.boundary(model.cardKey)
  const side = (v: J | undefined, title: string, output: boolean) => {
    const entries = isObj(v) ? v.entries() : []
    return (
      <Section title={title}>
        {!entries.length && <Row><span className="secondary">ありません</span></Row>}
        {entries.map(([name, ep]) => (
          <Row key={name}>
            <div className="row gap8 center">
              <span className="mono ellipsis" style={{ width: 140 }}>{name}</span>
              <Icon name={output ? 'arrow.left' : 'arrow.right'} className="secondary" />
              <span className="callout mono secondary">{`${str(get(ep, 'card')) ?? '?'}.${str(get(ep, 'port')) ?? '?'}`}</span>
            </div>
          </Row>
        ))}
      </Section>
    )
  }
  return (
    <Form>
      {side(get(b, 'inputs'), '入力ポート', false)}
      {side(get(b, 'outputs'), '出力ポート', true)}
      <Section footer="Card in Card は externals にあるこのプロジェクト用のコピーです。ポートの対応はその tsugumi.json の boundary にあります。中に入ると Card と接続を編集できます。">
        <Value label="external" value={model.session.cardExternal(model.cardKey) ?? ''} />
      </Section>
    </Form>
  )
}

/** Free text describing intent and notes, kept in `_meta.json` (`context`). */
function ContextCategory({ model }: { model: CardEditorModel }) {
  return (
    <div className="col gap8 fill pad12">
      <div className="callout secondary">処理の意図や補足を文章で残します（_meta.json）。チャットで Card を参照したときにも使われます。</div>
      {model.definitionId === undefined ? <div className="secondary">定義を持たない Card には追加コンテキストを置けません。</div> : (
        <textarea className="field flex1 cse-context" value={str(get(model.meta, 'context')) ?? ''} onChange={(e) => {
          const v = e.target.value
          model.update((m) => {
            const meta: JObj = isObj(m.meta) ? m.meta : obj()
            meta.set('context', v || undefined)
            m.meta = meta.size ? meta : undefined
          })
        }} />
      )}
    </div>
  )
}

// ---- Sources -----------------------------------------------------------------------

interface FileNode {
  name: string
  path: string
  children?: FileNode[]
}

export function fileIcon(name: string): string {
  switch (name.split('.').pop()) {
    case 'py': return 'chevron.left.forwardslash.chevron.right'
    case 'cc': case 'hh': case 'cpp': case 'hpp': case 'h': return 'curlybraces'
    case 'nix': return 'snowflake'
    case 'fbs': return 'tablecells'
    case 'json': return 'doc.badge.gearshape'
    case 'sh': return 'terminal'
    default: return 'doc.text'
  }
}

function fileTree(rel: string[], base: string): FileNode[] {
  const dirs = new Map<string, string[]>()
  const leaves: string[] = []
  for (const r of rel) {
    const i = r.indexOf('/')
    if (i >= 0) dirs.set(r.slice(0, i), [...(dirs.get(r.slice(0, i)) ?? []), r.slice(i + 1)])
    else leaves.push(r)
  }
  return [
    ...[...dirs.keys()].sort().map((d) => ({ name: d, path: base + d + '/', children: fileTree(dirs.get(d)!, base + d + '/') })),
    ...leaves.sort().map((l) => ({ name: l, path: base + l })),
  ]
}

const READ_ONLY_WHY = '接続から生成されるファイルです（手で編集しません）'

/** A short "RO" mark for files that are not edited here; hover tells why. */
export function ReadOnlyMark({ why }: { why: string }) {
  return <span className="ro-mark" title={why} aria-label="読み取り専用">RO</span>
}

function FileRow({ name, icon, depth, folder, selected, edited, readOnly, onClick, onContextMenu, help }: {
  name: string; icon: string; depth: number; folder?: boolean; selected: boolean; edited?: boolean; readOnly?: boolean
  onClick: () => void; onContextMenu?: (e: React.MouseEvent) => void; help?: string
}) {
  return (
    <div role="button" tabIndex={-1} aria-pressed={selected} className={`ft-row${selected ? ' selected' : ''}`} style={{ paddingLeft: 8 + depth * 14 }}
      onClick={onClick} onContextMenu={onContextMenu} title={help}>
      <span className="ft-chevron">{folder !== undefined && <Icon name={folder ? 'chevron.down' : 'chevron.right'} size={9} />}</span>
      <Icon name={icon} style={{ color: folder !== undefined ? 'var(--accent)' : 'var(--secondary)' }} />
      <span className="ellipsis flex1">{name}</span>
      {readOnly && <ReadOnlyMark why={READ_ONLY_WHY} />}
      {edited && <span className="edited-dot" />}
    </div>
  )
}

const GENERATED = 'generated:'

/** File tree, tabs and editor for a definition's sources, `_outputs.fbs` and
 *  `default.nix`, with formatting, errors in place and the generated inputs.
 *  The open files, the shown one and the expanded folders live on the model,
 *  so switching categories or applying keeps them. */
function SourceCategory({ model, only }: { model: CardEditorModel; only?: string }) {
  useObserve(model, prefs)
  const session = model.session
  const prefix = session.definitionDir(model.cardKey) ?? ''
  const files = model.files.filter((f) => !only || f.endsWith('/' + only))
  const [showNew, setShowNew] = useState(false)
  const [renaming, setRenaming] = useState<string>()
  const [formatting, setFormatting] = useState(false)
  const [formatMessage, setFormatMessage] = useState<string>()
  const [formatMarkers, setFormatMarkers] = useState<EditorMarker[]>([])
  const [generated, setGenerated] = useState<Record<string, string>>({})
  const [generatedError, setGeneratedError] = useState<string>()
  const shown = model.shownFile

  useEffect(() => {
    if (only) {
      if (files[0]) model.openFile(files[0])
    } else if (shown === undefined || shown.endsWith('/default.nix')) {
      const f = files.find((x) => x.endsWith('.py') || x.endsWith('.cc')) ?? files[0]
      if (f) model.openFile(f)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  // Reads the shown file (again after a reload).
  useEffect(() => {
    setFormatMarkers([])
    setFormatMessage(undefined)
    if (shown && !shown.startsWith(GENERATED) && model.files.includes(shown)) void model.text(shown).catch(() => {})
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [shown, model.generation])

  useEffect(() => {
    if (!shown?.startsWith(GENERATED)) return
    const rpc = session.rpc
    if (!rpc) return
    let alive = true
    void (async () => {
      try {
        const rev = session.rev
        const list = await rpc.generatedContracts(path(model.cardKey), rev)
        const out: Record<string, string> = {}
        for (const f of list) {
          const p = s(f.path)
          const name = p.split('/').pop() ?? p
          if (!name.endsWith('.fbs')) continue
          out[name] = new TextDecoder().decode(await rpc.readFile(p, rev))
        }
        if (!alive) return
        setGenerated((g) => ({ ...g, ...out }))
        setGeneratedError(undefined)
      } catch (e) {
        if (alive) setGeneratedError('生成できませんでした（保存されていない変更や未解決の型があると生成できません）: ' + errorText(e))
      }
    })()
    return () => {
      alive = false
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [shown, session.rev])

  const format = async (p: string) => {
    const rpc = session.rpc
    const source = model.loadedText(p)
    if (!rpc || source === undefined || formatting) return
    setFormatting(true)
    try {
      const r = await rpc.format(p, source)
      if (!r.diagnostics.length) {
        model.setText(p, s(r.text))
        setFormatMessage('整形しました')
        setFormatMarkers([])
      } else {
        setFormatMarkers(r.diagnostics.map((d) => ({ offset: d.offset ?? 0n, message: s(d.message) })))
        setFormatMessage('構文エラーがあります（テキストは変更していません）')
      }
    } catch (e) {
      setFormatMessage(e instanceof RPCFailure && e.kind === D.FailureKind.unsupported ? e.diagnostic ?? 'この形式の整形には対応していません' : errorText(e))
    }
    setFormatting(false)
  }

  const menu = (e: React.MouseEvent, n: FileNode) => showMenu(e, [
    { label: '開く', action: () => model.openFile(n.path) },
    { label: '名前を変更…', action: () => setRenaming(n.path) },
    { label: '削除', destructive: true, action: () => { model.removeFile(n.path); model.closeFile(n.path) } },
  ])

  const tree = (nodes: FileNode[], depth: number): ReactNode[] => nodes.flatMap((n) => {
    if (n.children) {
      const expanded = !model.collapsedFolders.has(n.path)
      return [
        <FileRow key={n.path} name={n.name} icon={expanded ? 'folder.fill' : 'folder'} depth={depth} folder={expanded} selected={false}
          onClick={() => model.update((m) => { if (expanded) m.collapsedFolders.add(n.path); else m.collapsedFolders.delete(n.path) })} />,
        ...(expanded ? tree(n.children, depth + 1) : []),
      ]
    }
    return [
      <FileRow key={n.path} name={n.name} icon={fileIcon(n.name)} depth={depth} selected={shown === n.path} edited={model.isEdited(n.path)}
        onClick={() => model.openFile(n.path)} onContextMenu={(e) => menu(e, n)} help={n.path} />,
    ]
  })

  const displayName = (f: string) => (f.startsWith(GENERATED) ? f.slice(GENERATED.length) : f.split('/').pop() ?? f)

  const header = (p: string, generatedFile: boolean) => {
    const rel = generatedFile ? p.slice(GENERATED.length) : p.slice(prefix.length)
    const parts = rel.split('/')
    return (
      <div className="src-head">
        <Icon name={generatedFile ? 'lock.doc' : fileIcon(p)} className="secondary" />
        <span className="callout mono headline">{parts[parts.length - 1] ?? rel}</span>
        <span className="caption secondary row center gap4 ellipsis">
          {model.definitionId}
          {parts.slice(0, -1).map((x, i) => <span key={i} className="row center gap4"><Icon name="chevron.right" size={8} />{x}</span>)}
        </span>
        {generatedFile && <ReadOnlyMark why={READ_ONLY_WHY} />}
        {model.created.has(p) ? <span className="caption2" style={{ color: 'var(--orange)' }}>新規</span>
          : model.isEdited(p) ? <span className="caption2" style={{ color: 'var(--orange)' }}>未適用</span> : null}
        <span className="spacer" />
        {formatMessage && <span className="caption secondary ellipsis">{formatMessage}</span>}
        {!generatedFile && (
          <Button small disabled={formatting || model.loadedText(p) === undefined} help={`ソースコードを整形（${prefs.label('formatSource')}）`}
            onClick={() => void format(p)}>整形</Button>
        )}
      </div>
    )
  }

  let pane: ReactNode
  if (shown?.startsWith(GENERATED)) {
    const name = shown.slice(GENERATED.length)
    pane = (
      <>
        {header(shown, true)}
        {generatedError && <div className="callout pad12 selectable" style={{ color: 'var(--red)' }}>{generatedError}</div>}
        <CodeEditor text={generated[name] ?? ''} language="fbs" editable={false} documentId={shown} className="src-editor" />
      </>
    )
  } else if (shown && model.files.includes(shown)) {
    const markers = [
      ...(session.resolution.get(model.cardKey)?.diagnostics ?? []).filter((d) => d.path === shown).map((d) => ({ offset: d.offset ?? 0n, message: d.message })),
      ...formatMarkers,
    ]
    const text = model.loadedText(shown)
    const load = model.loads.get(shown)
    pane = (
      <>
        {header(shown, false)}
        {text !== undefined ? (
          <CodeEditor text={text} language={shown.split('.').pop()} markers={markers} documentId={shown} className="src-editor"
            onChange={(t) => {
              model.setText(shown, t)
              if (formatMarkers.length) setFormatMarkers([])
            }} />
        ) : load?.state === 'failed' ? (
          // Never an editable empty file: the content is unknown.
          <Empty title="ファイルを読み込めませんでした" icon="exclamationmark.triangle" description={<span className="wrap-pre">{`内容が不明なため、編集・名前変更・保存はできません。\n${load.error}`}</span>}>
            <Button onClick={() => void model.text(shown).catch(() => {})}>再試行</Button>
          </Empty>
        ) : (
          <div className="center-fill col gap8"><Spinner /><span className="secondary">読み込んでいます…</span></div>
        )}
        {markers.length > 0 && (
          <div className="src-markers">
            {markers.map((m, i) => <span key={i} className="label caption selectable" style={{ color: 'var(--red)' }}><Icon name="xmark.octagon" />{m.message}</span>)}
          </div>
        )}
      </>
    )
  } else if (only && !files.length) {
    pane = (
      <Empty title="default.nix がありません" icon="snowflake" description="処理 Card の実行環境は default.nix で定義します。">
        <Button onClick={() => {
          model.createFile('default.nix', '{ pkgs, inputs, params }:\n{\n  package = inputs.tsugumi.pythonCard { src = ./src; };\n}\n')
          model.openFile(prefix + 'default.nix')
        }}>default.nix を作成</Button>
      </Empty>
    )
  } else {
    pane = <Empty title="ファイルを選んでください" icon="doc.text" />
  }

  const editor = (
    <div key="editor" className="src-pane" onKeyDown={(e) => {
      if (e.nativeEvent.isComposing || !shown || shown.startsWith(GENERATED)) return
      if (prefs.action(e.nativeEvent, false) === 'formatSource') {
        e.preventDefault()
        e.stopPropagation()
        void format(shown)
      }
    }}>
      {!only && model.openFiles.length > 1 && (
        <div className="src-tabs" role="tablist">
          {model.openFiles.map((f) => (
            <div key={f} role="tab" aria-selected={shown === f} className={`src-tab${shown === f ? ' shown' : ''}`} title={f} onClick={() => model.openFile(f)}>
              <Icon name={f.startsWith(GENERATED) ? 'lock.doc' : fileIcon(f)} size={11} />
              <span className="caption ellipsis">{displayName(f)}</span>
              {model.isEdited(f) && <span className="edited-dot" title="未適用の変更" />}
              <button type="button" className="src-tab-close" title="タブを閉じる（内容は保持されます）" aria-label="タブを閉じる"
                onClick={(e) => { e.stopPropagation(); model.closeFile(f) }}>
                <Icon name="xmark" size={8} />
              </button>
            </div>
          ))}
        </div>
      )}
      {pane}
    </div>
  )

  return (
    <div className="src-view">
      {only ? editor : (
        <HSplit sizes={[220, null]} min={[170, 320]} storageKey="tsugumi.sourceSplit">
          {[
            <div key="files" className="src-files">
              <div className="scroll flex1 src-tree">
                {tree(fileTree(files.map((f) => f.slice(prefix.length)), prefix), 0)}
                {model.kind === 'code' && (
                  <>
                    <div className="src-section caption">生成</div>
                    {['_inputs.fbs', '_params.fbs'].map((n) => (
                      <FileRow key={n} name={n} icon="lock.doc" depth={0} selected={shown === GENERATED + n} readOnly onClick={() => model.openFile(GENERATED + n)} />
                    ))}
                  </>
                )}
              </div>
              <div className="row gap4 pad6 border-top">
                <Button kind="borderless" iconOnly icon="plus" help="ファイルを作成" onClick={() => setShowNew(true)} />
                <Button kind="borderless" iconOnly icon="minus" help="表示中のファイルを削除" disabled={!shown || shown.startsWith(GENERATED)}
                  onClick={() => { if (shown && !shown.startsWith(GENERATED)) { model.removeFile(shown); model.closeFile(shown) } }} />
              </div>
            </div>,
            editor,
          ]}
        </HSplit>
      )}
      {showNew && <Prompt title="ファイルを作成" initial="src/" placeholder="パス（例: src/util.py）" confirm="作成" onCancel={() => setShowNew(false)}
        onConfirm={(v) => { model.createFile(v); model.openFile(prefix + v) }} />}
      {renaming && <Prompt title="名前を変更" initial={renaming.slice(prefix.length)} placeholder="新しいパス" confirm="変更" onCancel={() => setRenaming(undefined)}
        onConfirm={(v) => {
          const from = renaming
          void model.rename(from, v).then((ok) => {
            if (!ok) return
            model.closeFile(from)
            model.openFile(prefix + v)
          })
        }} />}
    </div>
  )
}
