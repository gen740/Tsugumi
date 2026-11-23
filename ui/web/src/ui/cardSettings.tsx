// Card settings: a local working copy of one Card's settings (its
// `_card.json`), metadata and sources. Nothing reaches the project until
// "Apply". Every Card has its own definition folder, so what is edited here is
// this Card only (inside a Card in Card: the Card of the external, this
// project's copy).
import { ReactNode, useEffect, useLayoutEffect, useRef, useState } from 'react'
import * as H from '../../shared/gen/tsugumi/host'
import { api } from '../api'
import { hostCall } from '../wire/host'
import { app } from '../model/app'
import { Observable, useObserve } from '../model/observable'
import { OutputCatalogResult, parseOutputCatalog } from '../model/outputCatalog'
import { pathChoices } from '../model/pathChoices'
import { prefs } from '../model/prefs'
import { portTypeName, CardComment, CardKey, CardParameter, commentDone, definitionKind, errorText, key, KIND_ICON, KIND_LABEL, paramText, parseParam, path, PortInfo, ProjectSession } from '../model/session'
import { CardEditorCategory, CATEGORY } from '../model/workspace'
import { BFBS } from '../../shared/bfbs'
import { arr, bool, clone, get, isObj, J, jeq, JNum, JObj, numText, obj, parse, serialize, str, tryParse } from '../../shared/json'
import { D, R, RPCFailure, s, sOpt, sha256Hex } from '../wire/rpc'
import { CardNodeView } from './cardNode'
import { Button, Confirm, Disclosure, Empty, Form, HSplit, Prompt, Row, Section, Select, showMenu, Spinner, TextField, Toggle, Value } from './controls'
import { CodeEditor, EditorMarker } from './editor'
import { CardArtifactsView, CardExecutionView } from './history'
import { CardLogView, DependenciesCategory } from './cardLog'
import { Icon } from './icons'
import { FolderPicker } from './sidebar'
import { SymbolPicker } from './symbolPicker'
import { L } from '../i18n'
import { splitCardNotes } from '../model/documents'
import { InlineParamEdit, RevisionEdit, RevisionEditNotice } from './revisionEdit'

/** Whether a source file's content is known. A file that could not be read
 *  is never shown or treated as an empty file. */
export type SourceLoad = { state: 'loading' } | { state: 'loaded' } | { state: 'failed'; error: string }

/** Local working copy of one Card. It follows what changes elsewhere while it
 *  is open (the Agent, the canvas): settings and files not touched here take
 *  the new content; changes on both sides require an explicit overwrite or
 *  reload choice. */
const inputParameters = (boolean: boolean) => `{ value = ${boolean ? 'false' : '0.0'}; }\n`

export class CardEditorModel extends Observable {
  definition?: J
  meta?: J
  /** Loaded or edited source text. */
  texts = new Map<string, string>()
  removed = new Set<string>()
  created = new Set<string>()
  renames = new Map<string, string>()
  private renameDestinations = new Map<string, string | undefined>()
  message?: string
  applying = false
  readonly revisionEdit = new RevisionEdit()
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
  private presetFiles = new Map<string, string>()
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
    return this.session.kind(this.cardKey) === 'subgraph' ? 'subgraph' : definitionKind(this.definition)
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

  rebindings = new Map<string, string>()

  /** Incident wires and boundary ends; every choice starts empty. */
  get draftEnds() {
    if (this.session.kind(this.cardKey) !== 'draft' || this.kind === 'draft') return []
    const scope = path(this.cardKey).slice(0, -1), id = path(this.cardKey).pop()
    const ends: { id: string; label: string; side: string; index?: number; boundary?: string }[] = []
    this.session.connections(scope).forEach((c, index) => {
      for (const side of ['from', 'to']) if (str(get(c, side, 'card')) === id) {
        const label = `${str(get(c, 'from', 'card'))}.${str(get(c, 'from', 'port'))} → ${str(get(c, 'to', 'card'))}.${str(get(c, 'to', 'port'))}`
        ends.push({ id: `${index}:${side}:${serialize(c, false)}`, label, side, index })
      }
    })
    const boundary = scope.length ? this.session.boundary(key(scope)) : undefined
    for (const side of ['inputs', 'outputs']) {
      const ports = get(boundary, side)
      if (isObj(ports)) for (const [name, ep] of ports.entries()) if (str(get(ep, 'card')) === id)
        ends.push({ id: `${side}:${name}:${serialize(ep, false)}`, label: `${side}.${name} → ${id}`, side, boundary: name })
    }
    return ends
  }

  get implementation() {
    const kind = definitionKind(this.definition)
    if (kind === 'input' && (this.presetFiles.get(this.prefix + 'params.nix') === inputParameters(true) ||
      this.session.resolution.get(this.cardKey)?.parameters?.some(p => p.name === 'value' && p.type === 'bool'))) return 'input_bool'
    return kind
  }

  implementAs(value: string) {
    if (value === 'input' || value === 'input_bool') {
      const type = value === 'input_bool' ? 'bool' : 'double'
      this.definition = obj([['type', 'input']])
      this.preset('_outputs.fbs', `namespace card;\ntable Value { value:${type}; }\ntable Outputs { value:Value (required); }\nroot_type Outputs;\n`)
      this.preset('params.nix', inputParameters(value === 'input_bool'))
    } else {
      this.definition = obj([['type', value]])
      for (const [file, text] of this.presetFiles)
        if (this.created.has(file) && this.texts.get(file) === text) this.removeFile(file)
      this.presetFiles.clear()
    }
    if (value === 'merge') this.setSetting('inputs', [obj([['name', 'a']]), obj([['name', 'b']])])
    this.changed()
  }

  private preset(relative: string, text: string) {
    const file = this.prefix + relative
    if (!this.prefix || this.session.sources.has(file) || this.removed.has(file)) return
    const untouched = this.created.has(file) && this.presetFiles.has(file) && this.texts.get(file) === this.presetFiles.get(file)
    if (!untouched && (this.created.has(file) || this.texts.has(file))) return
    this.createFile(relative, text)
    this.presetFiles.set(file, text)
  }

  /** Source files of the definition (documents excluded), with local changes. */
  get files(): string[] {
    const prefix = this.prefix
    if (!prefix) return []
    const docs = new Set([prefix + '_card.json', prefix + 'README.md', prefix + '_comments.json'])
    const all = new Set([...this.session.sourcePaths.filter((p) => p.startsWith(prefix) && !docs.has(p)), ...this.created])
    for (const p of this.removed) all.delete(p)
    return [...all].sort()
  }

  /** Changes whenever what this copy follows changes (the settings, the files' hashes). */
  get followToken(): string {
    const prefix = this.prefix
    const files = this.session.sourcePaths.filter((p) => p.startsWith(prefix)).map((p) => `${p}=${this.session.sources.get(p)?.hash ?? '-'}`)
    const ser = (v: J | undefined) => (v === undefined ? '' : serialize(v, false))
    return [String(this.session.rev), ser(this.session.cardDefinition(this.cardKey)), ser(this.session.cardMeta(this.cardKey))].join('\u0001') + files.join(';')
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
    this.presetFiles.clear()
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
    this.renames = new Map()
    this.renameDestinations = new Map()
    this.rebindings.clear()
    this.message = undefined
    this.conflicts = []
    this.revisionEdit.reload(this.session.rev)
    this.changed()
  }

  /** Untouched editors may reload; an open working copy is never locally merged. */
  async follow(whileApplying = false) {
    if ((!this.applying || whileApplying) && !this.unapplied) this.reload()
  }

  /** The Nix definition (default.nix), which Source edits. */
  openDefinition() {
    const prefix = this.session.definitionDir(this.cardKey)
    if (prefix === undefined) return
    this.category = 'source'
    this.openFile(prefix + 'default.nix')
  }

  /** The run and invocation the Log category shows; undefined follows this Card's latest. */
  logSelection?: { runId: string; invocation?: bigint }

  openLog(runId: string, invocation?: bigint) {
    this.logSelection = { runId, invocation }
    this.category = 'log'
    this.changed()
  }

  openSource(source: string, line: number) {
    const prefix = this.session.definitionDir(this.cardKey)
    if (!prefix || !Number.isSafeInteger(line) || line < 1 || source.startsWith('/') || source.split('/').some((p) => p === '..' || p === '.')) return
    this.category = 'source'
    this.openFile(prefix + source, line)
  }

  sourceLine?: number

  openFile(p: string, line?: number) {
    if (!this.openFiles.includes(p)) this.openFiles = [...this.openFiles, p]
    this.shownFile = p
    this.sourceLine = line
    this.changed()
  }

  closeFile(p: string) {
    const i = this.openFiles.indexOf(p)
    if (i < 0) return
    this.openFiles = this.openFiles.filter((f) => f !== p)
    if (this.shownFile === p) {
      this.shownFile = this.openFiles.length ? this.openFiles[Math.min(i, this.openFiles.length - 1)] : undefined
      this.sourceLine = undefined
    }
    this.changed()
  }

  /** Reads a source file once (later calls return the working copy).
   *  Throws when it cannot be read; nothing is recorded as its content then. */
  async text(p: string): Promise<string> {
    const have = this.texts.get(p)
    if (have !== undefined) return have
    this.loads.set(p, { state: 'loading' })
    this.changed()
    const source = this.renames.get(p) ?? p
    const hash = this.session.sources.get(source)?.hash
    try {
      const t = await this.session.sourceText(source)
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
    if (!this.created.has(p)) this.presetFiles.delete(p)
    this.removed.delete(p)
    this.created.add(p)
    const hash = this.session.sources.get(p)?.hash
    if (hash) this.baseHashes.set(p, hash)
    this.texts.set(p, text)
    this.loads.set(p, { state: 'loaded' })
    this.changed()
  }

  /** Stage a guarded move; renaming never decodes the source bytes. */
  async rename(p: string, rel: string): Promise<boolean> {
    const target = this.prefix + rel
    if (target === p) return true
    this.presetFiles.delete(p)
    this.presetFiles.delete(target)
    const origin = this.renames.get(p) ?? p
    const hash = this.baseHashes.get(p) ?? this.session.sources.get(origin)?.hash
    const text = this.texts.get(p), base = this.baseTexts.get(p), load = this.loads.get(p)
    const fresh = this.created.has(p) && !this.renames.has(p)
    this.created.delete(p); this.renames.delete(p); this.renameDestinations.delete(p)
    this.texts.delete(p); this.baseTexts.delete(p); this.loads.delete(p)
    if (!fresh) this.removed.add(origin)
    if (target === origin) this.removed.delete(origin)
    else { this.created.add(target); if (!fresh) this.renames.set(target, origin) }
    if (fresh) this.created.add(target)
    if (text !== undefined) this.texts.set(target, text)
    if (base !== undefined) this.baseTexts.set(target, base)
    if (load) this.loads.set(target, load)
    if (hash) { this.baseHashes.set(target, hash); this.baseHashes.set(origin, hash) }
    this.renameDestinations.set(target, this.session.sources.get(target)?.hash)
    this.changed()
    return true
  }

  removeFile(p: string) {
    this.renames.delete(p)
    this.renameDestinations.delete(p)
    const hash = this.session.sources.get(p)?.hash
    if (hash && !this.baseHashes.has(p)) this.baseHashes.set(p, hash)
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

  get recoveryText(): string {
    const notes = splitCardNotes(this.meta)
    return `_card.json\n${this.definition === undefined ? '' : serialize(this.definition)}\nREADME.md\n${notes.readme ?? ''}\n_comments.json\n${notes.comments === undefined ? '' : serialize(notes.comments)}` +
      [...this.texts].map(([p, t]) => `\n${p}\n${t}`).join('') + [...this.removed].map((p) => `\n- ${p}`).join('')
  }

  /** Copies the working copy into the project as one undoable edit (saved at once). */
  async apply(overwrite = false): Promise<boolean> {
    const session = this.session
    if (this.applying) return false
    if (!session.isEditable) {
      this.message = L('実行中は Card を変更できません。実行が終わってから適用してください。')
      this.changed()
      return false
    }
    this.applying = true
    this.changed()
    const ends = this.draftEnds
    if (ends.some(e => !this.rebindings.get(e.id)?.trim())) {
      this.message = L('各 wire の実ポート名を指定してください（ポート欄）。')
      this.applying = false
      this.changed()
      return false
    }
    const bindings = new Map(this.rebindings)
    const def = clone(this.definition), m = clone(this.meta), k = this.cardKey, prefix = this.prefix
    const sentTexts = new Map(this.texts), sentCreated = new Set(this.created)
    const moves = new Map(this.renames), destinations = new Map(this.renameDestinations)
    const changedTexts = [...this.texts].filter(([p, v]) => this.baseTexts.get(p) !== v || this.created.has(p) && !moves.has(p))
    const gone = [...this.removed].filter(p => !moves.has(p) && ![...moves.values()].includes(p))
    const bases = new Map(this.baseHashes)
    const ok = await this.revisionEdit.apply(session, (x) => {
      const edits: H.ProjectEditT[] = []
      if (def !== undefined) edits.push(x.jsonEdit(prefix + '_card.json', this.revisionEdit.jsonBase(prefix + '_card.json', this.baseDefinition, overwrite), def))
      edits.push(...x.notesEdits(prefix, this.revisionEdit.jsonBase(prefix + '#notes', this.baseMeta, overwrite), m))
      for (const [to, from] of moves) {
        const hash = this.revisionEdit.hashBase(from, bases.get(to), overwrite)
        if (!hash) throw new Error('Missing source hash: ' + from)
        const destination = this.revisionEdit.hashBase(to, destinations.get(to), overwrite)
        edits.push(new H.ProjectEditT(H.ProjectEditValue.ProjectFileMove,
          new H.ProjectFileMoveT(from, hash, to, destination ?? null, !destination)))
      }
      for (const [p, text] of changedTexts) {
        const hash = this.revisionEdit.hashBase(moves.get(p) ?? p, bases.get(p), overwrite)
        edits.push(new H.ProjectEditT(H.ProjectEditValue.ProjectFilePut,
          new H.ProjectFilePutT(p, hash ?? null, !hash, Array.from(new TextEncoder().encode(text)))))
      }
      for (const p of gone) {
        const hash = this.revisionEdit.hashBase(p, bases.get(p), overwrite)
        if (!hash) throw new Error('Missing source hash: ' + p)
        edits.push(new H.ProjectEditT(H.ProjectEditValue.ProjectFileRemove, new H.ProjectFileRemoveT(p, hash)))
      }
      x.appendEdits(edits)
      const scope = path(k).slice(0, -1)
      if (ends.some(e => e.index !== undefined)) x.updateGraph(scope, g => {
        const conns = arr(g.get('connections')) ?? []
        for (const e of ends) if (e.index !== undefined) {
          const ep = get(conns[e.index], e.side)
          if (isObj(ep)) ep.set('port', bindings.get(e.id)!.trim())
        }
      })
      if (ends.some(e => e.boundary !== undefined)) x.updateExternal(x.projectPrefix(scope)!, ext => {
        for (const e of ends) if (e.boundary !== undefined) {
          const ep = arr(get(ext.project, 'boundary', e.side))?.find(port => str(get(port, 'name')) === e.boundary)
          if (isObj(ep)) ep.set('port', bindings.get(e.id)!.trim())
        }
      })

    })
    if (!ok) { this.applying = false; this.changed(); return false }
    const laterDef = clone(this.definition), laterMeta = clone(this.meta)
    const laterMoves = new Map(this.renames), laterDestinations = new Map(this.renameDestinations)
    const laterTexts = new Map(this.texts), laterRemoved = new Set(this.removed), laterCreated = new Set(this.created)
    const savedCurrent = this.revisionEdit.savedRev === session.rev
    // Reload the confirmed base, then put only edits made after Send back on it.
    this.reload(false)
    if (!jeq(laterDef, def)) { this.definition = laterDef; this.baseDefinition = def }
    if (!jeq(laterMeta, m)) { this.meta = laterMeta; this.baseMeta = m }
    for (const [p, t] of laterTexts) {
      const sent = sentTexts.get(p)
      if (savedCurrent || t !== sent) {
        this.texts.set(p, t)
        if (sent !== undefined) this.baseTexts.set(p, sent)
        this.loads.set(p, { state: 'loaded' })
        const hash = sent !== undefined && changedTexts.some(([path]) => path === p) ? await sha256Hex(new TextEncoder().encode(sent)) : bases.get(p)
        if (hash) this.baseHashes.set(p, hash)
      }
    }
    const savedOrigins = new Set(moves.values())
    this.removed = new Set([...laterRemoved].filter(p => !gone.includes(p) && !savedOrigins.has(p)))
    for (const [to, from] of laterMoves) {
      if (moves.get(to) === from) continue
      const savedFrom = [...moves].find(([, origin]) => origin === from)?.[0] ?? from
      this.renames.set(to, savedFrom); this.renameDestinations.set(to, laterDestinations.get(to)); this.removed.add(savedFrom)
      const hash = session.sources.get(savedFrom)?.hash ?? bases.get(to)
      if (hash) this.baseHashes.set(to, hash)
    }
    for (const [to, from] of moves) {
      if (![...laterMoves.values()].includes(from) && !laterCreated.has(to)) this.removed.add(to)
    }
    this.created = new Set([...laterCreated].filter(p => !sentCreated.has(p)))
    for (const p of sentTexts.keys()) if (!laterTexts.has(p) && !gone.includes(p)) this.removed.add(p)
    this.applying = false
    this.message = L('適用しました。')
    this.changed()
    return true
  }
}

function categories(m: CardEditorModel): CardEditorCategory[] {
  switch (m.kind) {
    case 'code': return ['general', 'source', 'ports', 'log', 'dependencies', 'executions', 'artifacts', 'notes']
    case 'input': return ['general', 'source', 'ports', 'log', 'executions', 'artifacts', 'notes']
    case 'draft': case 'invalid': return ['general', 'notes']
    case 'subgraph': return ['general', 'ports', 'executions', 'notes']
    default: return ['general', 'ports', 'log', 'executions', 'artifacts', 'notes']
  }
}

export function CardSettingsEditor({ session, card, initial, file, onClose }: { session: ProjectSession; card: CardKey; initial?: CardEditorCategory; file?: 'default.nix'; onClose: () => void }) {
  const [model, setModel] = useState(() => {
    const m = new CardEditorModel(session, card)
    m.category = initial ?? 'general'
    if (file === 'default.nix') m.openDefinition()
    return m
  })
  useObserve(model, session)
  const [confirmReload, setConfirmReload] = useState(false)
  // The window was pointed at another Card: follow it.
  useEffect(() => {
    if (model.cardKey === card) return
    const m = new CardEditorModel(session, card)
    m.category = model.category
    setModel(m)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [card])
  // What changes elsewhere while the window is open is taken in at once.
  const token = model.followToken
  useEffect(() => { void model.follow() }, [token, model])

  if (!model.exists && !model.unapplied) {
    return <Empty title={L('Card が見つかりません')} icon="questionmark.square.dashed" description={L('{0} は削除されたか、まだ保存されていません。', model.cardKey)} />
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
      <nav className="cse-sidebar" aria-label={L('カテゴリ')}>
        {cats.map((c) => (
          <button key={c} type="button" className={`nav-row${c === category ? ' selected' : ''}`} aria-current={c === category}
            onClick={() => model.update((m) => { m.category = c })}>
            <Icon name={CATEGORY[c].icon} />{CATEGORY[c].title}
          </button>
        ))}
      </nav>
      <div className="cse-main">
        {/* The preview belongs to General; editors, ports and history use the whole height. */}
        {category === 'general' && <CardPreview session={session} model={model} height={170} />}
        <div className="cse-content">
          {category === 'general' && <GeneralCategory model={model} />}
          {category === 'source' && <SourceCategory model={model} />}
          {category === 'ports' && (model.kind === 'subgraph' ? <BoundaryCategory model={model} /> : <PortsCategory model={model} />)}
          {category === 'log' && <CardLogView session={session} model={model} />}
          {category === 'dependencies' && <DependenciesCategory model={model} />}
          {category === 'executions' && <CardExecutionView session={session} model={model} />}
          {category === 'artifacts' && <CardArtifactsView session={session} cardKey={model.cardKey} />}
          {category === 'notes' && <NotesCategory model={model} />}
        </div>
        <RevisionEditNotice edit={model.revisionEdit} text={() => model.recoveryText}
          overwrite={() => void model.apply(true)} reload={() => model.reload()} />
        <div className="cse-footer">
          {model.unapplied && <span className="label" style={{ color: 'var(--orange)' }}><Icon name="pencil.circle.fill" />{L('未適用の変更')}</span>}
          {model.message ? <span className="callout clamp2 selectable" style={{ color: session.saveError ? 'var(--red)' : 'var(--secondary)' }}>{model.message}</span>
            : !session.isEditable ? <span className="label secondary"><Icon name="lock.fill" />{L('実行中は変更を適用できません')}</span> : null}
          <span className="spacer" />
          {model.applying && <Spinner small />}
          <Button onClick={() => (model.unapplied ? setConfirmReload(true) : model.reload())}>{L('再読込')}</Button>
          <Button kind="prominent" guide="apply" disabled={!model.unapplied || model.applying || !session.isEditable} help="⌘S" onClick={() => void model.apply()}>{L('適用')}</Button>
          <Button help={L('未適用の変更は適用してから閉じます')} onClick={() => void close()}>{L('閉じる')}</Button>
        </div>
      </div>
      {confirmReload && <Confirm title={L('未適用の変更を破棄して読み込み直しますか？')} confirm={L('破棄して再読込')} onConfirm={() => model.reload()} onCancel={() => setConfirmReload(false)} />}
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
    notice = <span className="cse-notice" style={{ color: 'var(--orange)' }}><Icon name="exclamationmark.triangle.fill" />{L('_outputs.fbs を読めません: {0}（出力ポートは表示していません）', edited.error)}</span>
  } else if (edited) {
    notice = <span className="cse-notice secondary"><Icon name="pencil.circle" />{L('編集中の _outputs.fbs（未適用・型は適用後に解決）')}</span>
  } else if (session.saving) {
    notice = <span className="cse-notice secondary"><Icon name="hourglass" />{L('解決中…')}</span>
  }
  return (
    <div ref={box} className={`cse-preview${prefs.showGrid ? ' grid' : ''}`} style={{ height, ['--grid' as string]: `${prefs.gridSize}px` }}
      onPointerDown={(e) => { if (e.target === e.currentTarget) setSelected(false) }}>
      <div ref={inner} className="cse-preview-card" style={{ transform: `scale(${scale})` }}
        onPointerDown={() => setSelected(true)}>
        <CardNodeView data={data} flow={session.flow} selected={selected} onToggleCollapse={() => setCollapsed(!collapsed)} />
      </div>
      {notice && <div className="cse-preview-notice">{notice}</div>}
      <span className="cse-preview-label caption2 secondary">{L('プレビュー')}</span>
    </div>
  )
}

// ---- Categories --------------------------------------------------------------------

/** Id, description, params, when it runs, and the worker's permissions. */
function GeneralCategory({ model }: { model: CardEditorModel }) {
  const session = model.session
  const def = model.definition
  // This tsugumid runs on this host: without the sandbox a Card is a process
  // of the user's, so turning it off is the user's knowing choice.
  const [confirmSandboxOff, setConfirmSandboxOff] = useState(false)
  const string = (n: string, fallback?: string) => ({
    value: str(get(def, n)) ?? fallback ?? '',
    onChange: (v: string) => model.setSetting(n, !v || v === fallback ? undefined : v),
  })
  // A Card's intent is in its README.md.
  const metaString = (n: string) => ({
    value: str(get(model.meta, n)) ?? '',
    onChange: (v: string) => model.update((x) => {
      const meta: JObj = isObj(x.meta) ? x.meta : obj()
      meta.set(n, v || undefined)
      x.meta = meta
    }),
  })
  const toggle = (n: string, label: string, fallback: boolean, help?: string, disabled = false) => (
    <Row label={label} help={help}>
      <Toggle checked={bool(get(def, n)) ?? fallback} disabled={disabled} onChange={(v) => model.setSetting(n, v === fallback ? undefined : v)} />
    </Row>
  )
  const numberField = (n: string, label: string, fallback: string, help?: string) => (
    <Row label={label} help={help}>
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
      <TextField value={get(def, n) === null ? '' : numText(get(def, n)) ?? ''} placeholder={L('無期限')} style={{ width: 140 }}
        onChange={(v) => model.setSetting(n, v.trim() ? new JNum(v.trim()) : undefined)} />
    </Row>
  )
  const kind = model.kind
  const params = kind === session.kind(model.cardKey) ? session.resolution.get(model.cardKey)?.parameters ?? [] : []
  const diags = session.diagnostics.filter(d => d.card?.join('/') === model.cardKey)
  const serviceSetting = string('executionService', 'auto')
  const serviceId = serviceSetting.value
  const service = session.executionServices.find((e) => s(e.id) === serviceId)
  const nativeSystem = s(service?.nativeSystem)
  const systems = service?.systems.map(s) ?? []
  const system = string('system', '')
  const unavailable = !!system.value && !systems.includes(system.value)
  const serviceLabel = (entry: typeof service) => {
    if (!entry) return session.executionServices.length ? serviceId : L('情報なし')
    const id = s(entry.id), native = s(entry.nativeSystem)
    if (id !== 'local') return entry.available ? id : L('{0}（接続未確認）', id)
    if (native.endsWith('-linux')) return `Linux (${native})`
    if (native.endsWith('-darwin')) return `macOS (${native})`
    return id
  }
  const sandbox = get(def, 'sandbox')
  const grants = get(def, 'grants')
  const limits = get(sandbox, 'limits')
  const setSandbox = (key: string, value: J | undefined) => {
    const next = isObj(sandbox) ? clone(sandbox) as JObj : obj()
    next.set(key, value)
    model.setSetting('sandbox', next.size ? next : undefined)
  }
  const resourceLimits = serviceId === 'auto' || service?.resourceLimits === true
  const resource = (name: string, label: string, example: string) => (
    <Row label={label}>
      <TextField name={name} value={numText(get(limits, name)) ?? str(get(limits, name)) ?? ''} placeholder={example} style={{ width: 140 }}
        onChange={(value) => {
          const text = value.trim()
          const next = isObj(limits) ? clone(limits) as JObj : obj()
          const number = name === 'cpu' ? /^[0-9]+(?:\.[0-9]+)?$/ : /^[0-9]+$/
          next.set(name, !text ? undefined : number.test(text) ? new JNum(text.replace(/^0+(?=\d)/, '')) : text)
          setSandbox('limits', next.size ? next : undefined)
        }} />
    </Row>
  )
  return (
    <Form>
      <Section>
        <Value label="ID" value={session.displayName(model.cardKey)} />
        <Value label={L('種類')} value={KIND_LABEL[kind]} />
        {def !== undefined && (
          <Row label={L('アイコン')}>
            <SymbolPicker symbol={str(get(def, 'symbol')) || undefined} inherited={KIND_ICON[kind]}
              onChange={(v) => model.setSetting('symbol', v)} />
          </Row>
        )}
        <Row label={L('概要')}>
          <TextField {...metaString('description')} multiline rows={2} style={{ width: '100%', minHeight: 38, maxHeight: 90 }} />
        </Row>
        {kind === 'draft' && (
          <Row label={L('内容')}>
            <TextField {...metaString('content')} multiline rows={10} placeholder={L('何を受け取り、何を出し、どう処理するかを書きます')} style={{ width: '100%', minHeight: 160 }} />
          </Row>
        )}
      </Section>
      {model.session.kind(model.cardKey) === 'draft' && <Section title={L('Draft を手動で実装')} footer={L('定義と wire の実ポート名を同じ適用で保存します。コード Card はソースと出力スキーマも作成してください。')}>
        <Row label={L('種類')}><Select value={model.implementation} onChange={v => model.implementAs(v)} options={
          ['draft', 'code', 'input', 'input_bool', 'trigger', 'merge', 'collect']
            .map(value => ({ value, label: value === 'input_bool' ? L('真偽値の入力') : value === 'code' ? L('コード') : value }))
        } /></Row>
      </Section>}
      {params.length > 0 && <Section title={L('パラメータ')}>
        {params.map(parameter => <ParamRow key={parameter.name} model={model} parameter={parameter} />)}
      </Section>}
      {kind !== 'subgraph' && kind !== 'draft' && kind !== 'invalid' && (
        <Section title={L('実行')}>
          {numberField('maxRuns', L('最大発火回数'), '1', L('実行リスト内の呼び出しはまとめて 1 回の発火として数えます。'))}
          {timeoutField('timeoutSeconds', L('実行の期限（秒）'))}
          {kind === 'code' && (
            <>
              {timeoutField('buildTimeoutSeconds', L('ビルドの期限（秒）'))}
              <Row label={L('ワーカーの条件式')} help={L('x はワーカーの grade です。例: x > 1、x & 0x10 != 0。空欄はすべての値を許可します。')}>
                <TextField value={str(get(get(def, 'workerCondition'), 'expression')) ?? ''} mono onChange={(value) => model.setSetting('workerCondition', value.trim() ? obj([['language', 'simple'], ['expression', value]]) : undefined)} />
              </Row>
              {session.executionServices.length > 1 || !service && session.executionServices.length ? (
                <Row label={L('実行サービス')}>
                  <Select {...serviceSetting} options={[
                    { value: 'auto', label: L('条件から自動選択') },
                    ...session.executionServices.map((entry) => ({ value: s(entry.id), label: serviceLabel(entry), disabled: !entry.available })),
                    ...(!service && serviceId !== 'auto' ? [{ value: serviceId, label: serviceId, disabled: true }] : []),
                  ]} />
                </Row>
              ) : <Value label={L('実行サービス')} value={serviceLabel(service)} />}
              {session.environment.state === 'ready' && session.environment.executionScopeUnavailable && (
                <Row><span className="caption">{L('このプロジェクトのフォルダで作成時刻を取得できないため、外部の実行サービスを使用できません。')}</span></Row>
              )}
              {serviceId !== 'local' && serviceId !== 'auto' && (!service || !service.available) && (
                <Row><span className="caption">{L('選択した実行サービスに接続できません。')}</span></Row>
              )}
              <Row label={L('CPU アーキテクチャ')} help={L('選択した実行サービスで利用できる CPU アーキテクチャを選びます。')}>
                <Select {...system} options={[
                  { value: '', label: nativeSystem ? L('この実行環境の既定（{0}）', nativeSystem) : L('この実行環境の既定') },
                  ...systems.map((value) => ({ value, label: value })),
                  ...(unavailable ? [{ value: system.value, label: system.value, disabled: true }] : []),
                ]} />
              </Row>
              {service && unavailable && (
                <Row><span className="label caption"><Icon name="exclamationmark.triangle" />{L('この実行環境では {0} を実行できません。', system.value)}</span></Row>
              )}
              {resourceLimits && <>
                <Row><span className="caption secondary">{L('CPU とメモリの上限を指定します。実行先のリソースは他のジョブと共有します。')}</span></Row>
                {resource('cpu', L('CPU 上限（コア）'), '0.5')}
                {resource('memoryMiB', L('メモリ上限（MiB）'), '1024')}
              </>}
              {limits !== undefined && <Row>
                {!resourceLimits && <span className="caption">{L('この実行サービスではリソースを指定できません。')}</span>}
                <Button kind="borderless" onClick={() => setSandbox('limits', undefined)}>{L('リソース指定を削除')}</Button>
              </Row>}
              {toggle('reuseWorker', L('worker の状態を残す'), false,
                L('オン: 読み込んだ worker を次の呼び出しや次の実行でも使い、状態（static 変数など）を残します。オフ: 呼び出しごとに新しい worker です。'),
                serviceId !== 'local' && serviceId !== 'auto' && bool(get(def, 'reuseWorker')) !== true)}
              {serviceId !== 'local' && serviceId !== 'auto' && bool(get(def, 'reuseWorker')) === true && (
                <Row><span className="caption">{L('外部の実行サービスでは worker の状態を残せません。')}</span></Row>
              )}
              <Row label={L('ファイルアクセスを制限')}>
                <Toggle checked={bool(get(sandbox, 'filesystem')) ?? true} disabled={serviceId !== 'local' && serviceId !== 'auto' && bool(get(sandbox, 'filesystem')) !== false}
                  onChange={(v) => (v ? setSandbox('filesystem', undefined) : setConfirmSandboxOff(true))} />
              </Row>
              {serviceId !== 'local' && serviceId !== 'auto' && bool(get(sandbox, 'filesystem')) === false && (
                <Row><span className="caption">{L('外部の実行サービスにはファイルアクセスの制限が必要です。')}</span></Row>
              )}
              {([
                ['pid', L('PID 名前空間を分離')],
                ['ipc', L('IPC 名前空間を分離')],
                ['identity', L('実行ユーザーと権限を分離')],
              ] as const).map(([key, label]) => (
                <Row key={key} label={label}>
                  <Toggle checked={bool(get(sandbox, key)) ?? true} onChange={(v) => setSandbox(key, v ? undefined : false)} />
                </Row>
              ))}
              {confirmSandboxOff && (
                <Confirm title={L('この Card のファイルアクセス制限を解除しますか？')} confirm={L('制限を解除する')}
                  message={L('アクセスできる範囲は実行環境の権限と共有フォルダに依存します。ネットワーク制限も実行環境によっては無効になります。')}
                  onConfirm={() => { setConfirmSandboxOff(false); setSandbox('filesystem', false) }} onCancel={() => setConfirmSandboxOff(false)} />
              )}
            </>
          )}
        </Section>
      )}
      {kind === 'code' && (
        <Section title={L('アクセス許可')}>
          {([
            ['network', L('ネットワークへのアクセスを許可する')],
            ['gui', L('GUI へのアクセスを許可する')],
          ] as const).map(([key, label]) => (
            <Row key={key} label={label}>
              <Toggle checked={bool(get(grants, key)) ?? false}
                onChange={(v) => setGrant(model, key, v ? true : undefined)} />
            </Row>
          ))}
          <Row><HostFoldersEditor model={model} remote={serviceId !== 'local' && serviceId !== 'auto'} /></Row>
        </Section>
      )}
      {diags.length > 0 && (
        <Section title={L('診断')}>
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

function setGrant(model: CardEditorModel, key: string, value: J | undefined) {
  const grants = get(model.definition, 'grants')
  const next = isObj(grants) ? clone(grants) as JObj : obj()
  next.set(key, value)
  model.setSetting('grants', next.size ? next : undefined)
}

/** Folders of the host this Card's worker may read (Experimental): each at
 *  /host/<name> in the worker, read-only unless allowed to write. Only the
 *  folder's path is kept in the project. */
const grantName = (name: string) => name.toUpperCase().replace(/[^A-Z0-9]/g, '_')

function HostFoldersEditor({ model, remote }: { model: CardEditorModel; remote: boolean }) {
  const folders = arr(get(get(model.definition, 'grants'), 'hostPaths')) ?? []
  const usable = app.status?.hostFolders ?? true
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
    setGrant(model, 'hostPaths', list.length ? list : undefined)
  }
  const add = (p: string) => {
    const taken = new Set(folders.map((f) => grantName(str(get(f, 'name')) ?? '')))
    const last = p.replace(/\/+$/, '').split('/').pop() ?? ''
    const base = [...last].map((c) => (/[A-Za-z0-9\-_.]/.test(c) ? c : '-')).join('')
    let name = !base || base === '.' || base === '..' ? 'folder' : base
    for (let n = 2; taken.has(grantName(name)); n++) name = `${base}-${n}`
    edit((l) => l.push(obj([['path', p], ['name', name]])))
  }
  return (
    <div className="col gap6 fill">
      <div className="row center">
        <span>{L('ホストのフォルダへのアクセスを許可する')}</span>
        <span className="spacer" />
        <Button disabled={!usable || remote} onClick={() => setPicking(true)}>{L('フォルダを追加…')}</Button>
      </div>
      {folders.map((f, i) => {
        const p = str(get(f, 'path')) ?? ''
        const ok = present[p] !== false
        return (
          <div key={i} className="row gap6 center host-folder">
            <Icon name={ok && usable ? 'folder' : 'folder.badge.questionmark'} style={{ color: ok && usable ? 'var(--secondary)' : 'var(--orange)' }} />
            <div className="col flex1 min0">
              <span className="callout ellipsis" title={p}>{p}</span>
              <span className="caption" style={{ color: ok && usable ? 'var(--secondary)' : 'var(--orange)' }}>
                {remote ? L('外部の実行サービスにはホストのフォルダを共有できません。') : !usable ? L('この Web UI では使えません（実行すると失敗します）') : ok ? `TSUGUMI_GRANT_${grantName(str(get(f, 'name')) ?? '')}` : L('このホストにはありません（実行すると失敗します）')}
              </span>
            </div>
            <Toggle kind="checkbox" label={L('書き込み')} help={L('オフ（デフォルト）: 読み取り専用')} checked={bool(get(f, 'write')) ?? false} disabled={remote}
              onChange={(v) => edit((l) => isObj(l[i]) && (l[i] as JObj).set('write', v ? true : undefined))} />
            <Button kind="borderless" iconOnly icon="minus.circle" help={L('削除')} onClick={() => edit((l) => l.splice(i, 1))} />
          </div>
        )
      })}
      {remote ? <div className="caption secondary">{L('外部の実行サービスにはホストのフォルダを共有できません。')}</div>
        : !usable && <div className="caption" style={{ color: 'var(--orange)' }}>{L('macOS 上の Web UI では、フォルダを Card に共有できません（Mac アプリの共通 VM が必要です）。この設定のある Card を実行すると失敗します。')}</div>}
      <div className="caption secondary">{L('Card は TSUGUMI_GRANT_<名前> からフォルダのパスを取得できます。共有 VM での追加・変更はプロジェクトを開き直すと反映されます。')}</div>
      {picking && <FolderPicker message={L('worker から読めるようにするフォルダを選んでください')} onClose={() => setPicking(false)} onPick={(p) => { setPicking(false); add(p) }} />}
    </div>
  )
}

function ParamRow({ model, parameter }: { model: CardEditorModel; parameter: CardParameter }) {
  const session = model.session
  const { name, type } = parameter
  const value = parameter.value
  const sweep = arr(get(model.definition, 'sweeps', name))
  const [draft, setDraft] = useState<string>()
  const [sweepDraft, setSweepDraft] = useState<string>()
  const [edit] = useState(() => new InlineParamEdit())
  useObserve(edit)

  const disabled = !session.isEditable || edit.applying || model.applying
  const pathInput = model.kind === 'input' && name === 'value' && type === 'string' &&
    session.resolution.get(model.cardKey)?.outputs.some((p) => p.kind === D.PortKind.Path)
  const folder = session.definitionDir(model.cardKey) ?? ''
  const entered = draft ?? str(value) ?? ''
  const target = entered.startsWith('project:')
    ? folder.split('/').slice(0, -3).map((part) => part + '/').join('') + entered.slice(8)
    : folder + entered
  const choices = pathInput && !sweep ? pathChoices(session.sourcePaths, folder) : []
  const begin = () => {
    if (draft === undefined) edit.begin(paramText(value), session.rev)
  }
  const clear = () => {
    setDraft(undefined)
    edit.begin(paramText(value), session.rev)
  }
  const save = async (text: string | undefined, overwrite = false) => {
    if (text !== undefined && await edit.commitParam(session, model.cardKey, name, parseParam(text, type), overwrite)) clear()
  }
  const set = (text: string) => {
    begin()
    setDraft(text)
    void save(text)
  }
  const setSweep = (values: J[] | undefined) => {
    const sweeps = clone(get(model.definition, 'sweeps'))
    const next = isObj(sweeps) ? sweeps : obj()
    next.set(name, values)
    model.setSetting('sweeps', next.size ? next : undefined)
  }
  const pendingSweepRemoval = !sweep && get(session.cardDefinition(model.cardKey), 'sweeps', name) !== undefined
  const valueDisabled = disabled || pendingSweepRemoval
  const sweepText = sweepDraft ?? serialize(sweep ?? [], false)
  return (
    <Row label={name}>
      <div>
        <div className="row gap6 center" data-guide={'param:' + name}>
          {sweep ? <TextField value={sweepText} disabled={disabled} onChange={(text) => {
            setSweepDraft(text)
            const values = arr(tryParse(text))
            if (values) setSweep(values)
          }} style={{ width: 220 }} />
            : type === 'bool'
            ? <Toggle checked={draft === undefined ? bool(value) ?? false : draft === 'true'} disabled={valueDisabled} onChange={v => set(String(v))} />
            : <>
              <TextField value={draft ?? paramText(value)} disabled={valueDisabled} onChange={text => { begin(); setDraft(text) }}
                onKeyDown={e => {
                  if (e.key !== 'Enter' || e.nativeEvent.isComposing || draft === undefined) return
                  e.stopPropagation()
                  void save(draft)
                }} style={{ width: 220 }} />
              {draft !== undefined && <Button disabled={valueDisabled} onClick={() => void save(draft)}>{L('保存')}</Button>}
            </>}
          {pathInput && !sweep && <Select value={choices.includes(entered) ? entered : ''} onChange={v => v && set(v)} disabled={valueDisabled} style={{ maxWidth: 220 }}
            help={L('プロジェクトのファイルかフォルダを選びます。')}
            options={[{ value: '', label: L('ファイルかフォルダを選択…'), disabled: true },
              ...choices.map(p => ({ value: p, label: p }))]} />}
          <span className="caption2 secondary nowrap">{type}</span>
        </div>
        {model.kind === 'input' && <div className="row gap6 center">
          <Toggle kind="checkbox" small checked={!!sweep} disabled={disabled || draft !== undefined} onChange={enabled => {
            setSweepDraft(undefined)
            setSweep(enabled ? [value] : undefined)
          }} label={L('値ごとに呼び出す')} help={L('並べた値を 1 つずつ別の呼び出しとして渡します（実行リスト）。')} />
        </div>}
        {sweep && <div className="caption secondary">
          {arr(tryParse(sweepText)) ? L('{0} 個の値を 1 つずつ別の呼び出しとして渡します（実行リスト）。', sweep.length)
            : L('値を JSON の配列で書いてください。例: [8, 12, 16]')}
        </div>}
        {pathInput && !sweep && <div className="caption secondary" style={{ overflowWrap: 'anywhere' }}>
          <div>{L('Card の定義フォルダからの相対パス。project: を付けると所属プロジェクトからの相対パスになります。')}</div>
          <div>{L('参照先: {0}', target)}</div>
        </div>}
        <RevisionEditNotice edit={edit} text={() => draft ?? ''}
          overwrite={() => void save(draft, true)} reload={clear} />
      </div>
    </Row>
  )
}

/** A port's purpose in the Card description. */
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
  const editable = def !== undefined && ['code', 'merge'].includes(model.kind)
  const inputs = arr(get(def, 'inputs')) ?? []
  const editInputs = (fn: (a: J[]) => void) => {
    const a = inputs.map((x) => clone(x))
    fn(a)
    model.setSetting('inputs', a.length ? a : undefined)
  }
  const keepable = (r?.outputs ?? []).filter((p) => p.kind === D.PortKind.Path)
  return (
    <Form>
      {model.draftEnds.length > 0 && <Section title={L('wire の再接続')} footer={L('入力 wire には実入力ポート、出力 wire には実出力ポートを指定します。候補が一つでも指定が必要です。')}>
        {model.draftEnds.map(e => <Row key={e.id} label={`${e.side === 'from' || e.side === 'outputs' ? L('出力ポート') : L('入力ポート')}: ${e.label}`}>
          <TextField value={model.rebindings.get(e.id) ?? ''} placeholder={L('実ポート名')} onChange={v => { model.rebindings.set(e.id, v); model.changed() }} />
        </Row>)}
      </Section>}
      <Section title={L('入力ポート')} footer={L('入力の型は接続元の出力から決まります（_inputs.fbs は生成され、手で書きません）。')}>
        {editable ? (
          <>
            {inputs.map((inp, i) => {
              const name = str(get(inp, 'name')) ?? ''
              return (
                <Row key={i}>
                  <div className="row gap8 center fill">
                    <TextField value={name} placeholder={L('名前')} mono help={L('入力ポートの名前')} style={{ width: 140 }}
                      onChange={(v) => editInputs((a) => isObj(a[i]) && (a[i] as JObj).set('name', v))} />
                    <TextField {...portNote(model, 'inputs', name)} placeholder={L('用途（何を受け取るか）')} style={{ flex: 1 }}
                      help={L('このポートの用途（README.md の説明）。Agent とほかの人への説明です。')} />
                    <span className="caption secondary nowrap">{portTypeName(r?.inputs.find((p) => p.name === name)) ?? L('未解決')}</span>
                    <Button kind="borderless" iconOnly icon="minus.circle" help={L('入力を削除')} onClick={() => editInputs((a) => a.splice(i, 1))} />
                  </div>
                </Row>
              )
            })}
            <Row><Button onClick={() => editInputs((a) => a.push(obj([['name', `in${a.length + 1}`]])))}>{L('入力を追加')}</Button></Row>
          </>
        ) : (
          <>
            {(r?.inputs ?? []).map((p) => <Value key={p.name} label={p.name} value={portTypeName(p) ?? L('未解決')} />)}
            {!(r?.inputs ?? []).length && <Row><span className="secondary">{L('入力はありません')}</span></Row>}
          </>
        )}
      </Section>
      <OutputPortsSection model={model} />
      {model.kind !== 'subgraph' && model.kind !== 'draft' && model.kind !== 'invalid' && (
        <Section title={L('記録と成果物')}>
          {([
            ['recordLogs', L('標準出力と標準エラーを記録')],
            ['recordIO', L('入出力の値を記録')],
            ['retainArtifact', L('出力ファイルと成果物を保持')],
          ] as const).map(([key, label]) => {
            const value = bool(get(def, key))
            return <Row key={key} label={label}>
              <Select value={value === undefined ? 'inherit' : value ? 'on' : 'off'}
                onChange={(v) => model.setSetting(key, v === 'inherit' ? undefined : v === 'on')}
                options={[{ value: 'inherit', label: L('プロジェクト設定に従う') },
                  { value: 'on', label: L('オン') }, { value: 'off', label: L('オフ') }]} />
            </Row>
          })}
          {/* Only Path outputs can be kept as artifacts. */}
          <Row label={L('成果物にする出力')}>
            {keepable.length === 0
              ? <span className="callout secondary">{r === undefined ? L('出力の型がまだ解決されていません') : L('Path の出力がないため、保持できる出力はありません')}</span>
              : <Select value={str(get(def, 'artifactOutput')) ?? ''} onChange={(v) => model.setSetting('artifactOutput', v || undefined)}
                  options={[{ value: '', label: L('なし') },
                    ...keepable.map((p) => ({ value: p.name, label: p.name }))]} />}
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
        setError(sOpt(res.error) ?? L('変更できませんでした'))
      }
    } catch (e) {
      setError(errorText(e))
    }
  }
  const ports: PortInfo[] = r?.outputs ?? []
  return (
    <Section title={L('出力ポート')} footer={L('出力ポートと型の正本は _outputs.fbs です（型はここで選べます。型そのものはソースで定義します）。')}>
      {ports.map((port) => (
        <Row key={port.name}>
          <div className="row gap8 center fill">
            <span className="mono ellipsis" style={{ width: 120, flex: 'none' }}>{port.name}</span>
            <TextField {...portNote(model, 'outputs', port.name)} placeholder={L('用途（何を出すか）')} style={{ flex: 1 }} />
            {!p ? (
              <span className="caption secondary nowrap">{portTypeName(port) ?? L('未解決')}</span>
            ) : (
              <Select value={port.kind === D.PortKind.Path ? 'string' : port.rootType ?? ''} style={{ maxWidth: 150 }}
                help={L('_outputs.fbs で定義した型から選びます（_outputs.fbs に書き戻します）')}
                onChange={(type) => void set(port.name, type, type === 'string' ? D.PortKind.Path : D.PortKind.FlatBuffer)}
                options={[...(port.kind !== D.PortKind.Path && port.rootType === undefined ? [{ value: '', label: L('未解決'), disabled: true }] : []),
                  { value: 'string', label: 'Path' },
                  ...(port.kind !== D.PortKind.Path && port.rootType !== undefined && !types.includes(port.rootType) ? [{ value: port.rootType, label: shortType(port.rootType) }] : []),
                  ...types.map((type) => ({ value: type, label: shortType(type) }))]} />
            )}
          </div>
        </Row>
      ))}
      {ports.length === 0 && <Row><span className="secondary">{L('出力はありません')}</span></Row>}
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
        {!entries.length && <Row><span className="secondary">{L('ありません')}</span></Row>}
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
      {side(get(b, 'inputs'), L('入力ポート'), false)}
      {side(get(b, 'outputs'), L('出力ポート'), true)}
      <Section footer={L('Card in Card は externals にあるこのプロジェクト用のコピーです。ポートの対応はその tsugumi.json の boundary にあります。中に入ると Card と接続を編集できます。')}>
        <Value label="external" value={model.session.cardExternal(model.cardKey) ?? ''} />
      </Section>
    </Form>
  )
}

/** Comments (what to change, for the Agent) above the added context (what the
 *  Card is for); a Card with no definition has only comments. */
function NotesCategory({ model }: { model: CardEditorModel }) {
  return (
    <div className="col fill min0">
      <div className="flex1 min0 col"><CommentsCategory session={model.session} card={model.cardKey} /></div>
      {model.kind !== 'subgraph' && (
        <div className="col border-top" style={{ height: 220 }}>
          <div className="headline" style={{ padding: '12px 12px 0' }}>{L('追加コンテキスト')}</div>
          <ContextCategory model={model} />
        </div>
      )}
    </div>
  )
}

/** Free text describing intent and notes in README.md. */
function ContextCategory({ model }: { model: CardEditorModel }) {
  return (
    <div className="col gap8 fill pad12">
      <div className="callout secondary">{L('処理の意図や補足を文章で残します（README.md）。チャットで Card を参照したときにも使われます。')}</div>
      {model.definitionId === undefined ? <div className="secondary">{L('定義を持たない Card には追加コンテキストを置けません。')}</div> : (
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

/** Whose comment this is, when it is not simply one of the user's. */
function CommentRecord({ comment: c }: { comment: CardComment }) {
  const parts = [
    c.author === 'agent' && L('Agent が書きました'),
    c.editedBy && c.editedBy !== (c.author ?? 'user') && commentDone(c.editedBy, L('編集')),
    !c.resolved && c.stateBy === 'agent' && L('Agent が開き直しました'),
  ].filter((x): x is string => !!x)
  if (!parts.length) return null
  return (
    <div className="row gap6 center caption">
      <span className="cse-comment-who"><Icon name="bubble.left" size={10} />Agent</span>
      <span className="secondary">{parts.join(L('・'))}</span>
    </div>
  )
}

/** What the user and the Agent write about this Card. Saved at once, not with Apply
 *  (during a run, when the run ends, as other edits): it says nothing about how it runs. */
function CommentsCategory({ session, card }: { session: ProjectSession; card: CardKey }) {
  useObserve(session)
  const [text, setText] = useState('')
  const [revisionEdit] = useState(() => new InlineParamEdit())
  useObserve(revisionEdit)
  const [editing, setEditing] = useState<{ id: string; text: string }>()
  const comments = session.comments(card)
  const open = comments.filter((c) => !c.resolved), resolved = comments.filter((c) => c.resolved)
  const add = async () => {
    const submitted = text
    try {
      await session.addComment(card, submitted)
      setText(current => current === submitted ? '' : current)
    } catch (e) { session.cardEditFailed(e) }
  }
  const beginCommentEdit = (id: string, text: string) => {
    const value = arr(get(session.cardMeta(card), 'comments'))?.find(c => str(get(c, 'id')) === id)
    revisionEdit.beginField(text, session, (session.definitionDir(card) ?? '') + '_comments.json', ['comments', obj([['id', id]])], value)
  }
  const saveEdit = async (overwrite = false) => {
    if (!editing) return
    const copy = editing
    if (await revisionEdit.commitField(session, copy.text, overwrite, value => {
      if (isObj(value) && copy.text.trim()) { value.set('text', copy.text.trim()); value.set('edited_by', 'user') }
      return value
    })) setEditing(undefined)
  }
  const submitting = (run: () => void) => (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && (e.metaKey || e.ctrlKey) && !e.nativeEvent.isComposing) {
      e.preventDefault()
      e.stopPropagation()
      run()
    }
  }
  const row = (c: CardComment) => (
    <div key={c.id} className="cse-comment col gap6" data-comment={c.id}>
      {editing?.id === c.id ? (
        <>
          <TextField multiline rows={3} autoFocus value={editing.text} onChange={(t) => setEditing({ id: c.id, text: t })} onKeyDown={submitting(() => void saveEdit())} />
          <div className="row end gap6">
            <Button small onClick={() => setEditing(undefined)}>{L('キャンセル')}</Button>
            <Button small kind="prominent" disabled={!editing.text.trim() || !session.connected} onClick={() => void saveEdit()}>{L('保存')}</Button>
          </div>
        </>
      ) : (
        <>
          <CommentRecord comment={c} />
          <div className="cse-comment-text selectable">{c.text}</div>
          {(c.resolution || c.cards.length > 0 || (c.resolved && commentDone(c.stateBy, L('解決')))) && (
            <div className="caption secondary selectable">
              <Icon name="checkmark" size={10} /> {commentDone(c.stateBy, L('解決')) ?? L('解決')}
              {c.resolution && `: ${c.resolution}`}
              {c.cards.length > 0 && L('（変更した Card: {0}）', c.cards.join('、'))}
            </div>
          )}
          <div className="row end gap6">
            <Button small disabled={!session.connected} onClick={() => { beginCommentEdit(c.id, c.text); setEditing({ id: c.id, text: c.text }) }}>{L('編集')}</Button>
            {c.resolved
              ? <Button small disabled={!session.connected} onClick={() => session.setCommentResolved(card, c.id, false)}>{L('開き直す')}</Button>
              : <Button small disabled={!session.connected} onClick={() => session.setCommentResolved(card, c.id, true)}>{L('解決')}</Button>}
            <Button small kind="borderless" iconOnly icon="trash" help={L('削除')} disabled={!session.connected} onClick={() => session.removeComment(card, c.id)} />
          </div>
        </>
      )}
    </div>
  )
  return (
    <div className="col gap8 pad12 scroll fill">
      <div className="callout secondary">{L('この Card をどう変えたいかを書きます。Canvas の「コメントから生成」で Agent が従います（_comments.json に保存され、版の履歴に残ります）。')}</div>
      <TextField multiline rows={3} value={text} onChange={setText} placeholder={L('例: y 軸を対数にしてください')} onKeyDown={submitting(add)} />
      <div className="row end">
        <Button kind="prominent" disabled={!text.trim() || !session.connected} help="⌘Return" onClick={add}>{L('コメントを追加')}</Button>
      </div>
      <RevisionEditNotice edit={revisionEdit} text={() => editing?.text ?? ''} overwrite={() => void saveEdit(true)} reload={() => {
        const current = comments.find((c) => c.id === editing?.id)
        setEditing(current ? { id: current.id, text: current.text } : undefined)
        beginCommentEdit(editing?.id ?? '', current?.text ?? '')
      }} />
      {editing && !comments.some((c) => c.id === editing.id) && row({ ...editing, resolved: false, cards: [] })}
      {open.map(row)}
      {resolved.length > 0 && <Disclosure label={L('解決済み（{0}）', resolved.length)}><div className="col gap6">{resolved.map(row)}</div></Disclosure>}
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

const READ_ONLY_WHY = L('Card のビルドが生成したファイルです（手で編集しません）')

/** A short "RO" mark for files that are not edited here; hover tells why. */
export function ReadOnlyMark({ why }: { why: string }) {
  return <span className="ro-mark" title={why} aria-label={L('読み取り専用')}>RO</span>
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

const SDK = 'sdk:'
const SDK_LOG = SDK + '/log'

/** What the last "build SDK" returned: the files of share/tsugumi/api/ by
 *  their shown name, or the build failure and its log. Held only while the
 *  editor is open. */
type SdkState = { building: boolean, files?: { id: string, name: string, text: string }[], failure?: string, log?: string, truncated?: boolean }

export function sdkBuild(result: R.EvaluateNixResultT): SdkState {
  if (result.failure) return { building: false, failure: sOpt(result.failure.diagnostic) ?? D.FailureKind[result.failure.kind], log: sOpt(result.log) }
  const directory = 'share/tsugumi/api/'
  const decoder = new TextDecoder()
  const files = result.files.flatMap(file => {
    const fullPath = s(file.path)
    if (!fullPath.startsWith(directory)) return []
    const name = fullPath.slice(directory.length)
    return [{ id: SDK + name, name, text: decoder.decode(Uint8Array.from(file.content)) }]
  })
  return { building: false, files, truncated: result.truncated }
}

/** File tree, tabs and editor for a definition's sources, `_outputs.fbs` and
 *  `default.nix`, with formatting, errors in place and the Card's generated
 *  API (the "SDK" section: what the builder placed under share/tsugumi/api/).
 *  The open files, the shown one and the expanded folders live on the model,
 *  so switching categories or applying keeps them. */
function SourceCategory({ model }: { model: CardEditorModel }) {
  useObserve(model, prefs)
  const session = model.session
  const prefix = session.definitionDir(model.cardKey) ?? ''
  const files = model.files
  const [showNew, setShowNew] = useState(false)
  const [renaming, setRenaming] = useState<string>()
  const [formatting, setFormatting] = useState(false)
  const [formatMessage, setFormatMessage] = useState<string>()
  const [formatMarkers, setFormatMarkers] = useState<EditorMarker[]>([])
  const [sdk, setSdk] = useState<SdkState>({ building: false })
  const shown = model.shownFile

  useEffect(() => {
    if (shown === undefined) {
      const f = files[0]
      if (f) model.openFile(f)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  // Reads the shown file (again after a reload).
  useEffect(() => {
    setFormatMarkers([])
    setFormatMessage(undefined)
    if (shown && !shown.startsWith(SDK) && model.files.includes(shown)) void model.text(shown).catch(() => {})
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [shown, model.generation])

  // Build the saved Card's API derivation with the current wiring.
  const buildSdk = async () => {
    const rpc = session.rpc
    if (!rpc || sdk.building) return
    setSdk({ building: true })
    try {
      const result = await rpc.evaluateNix(path(model.cardKey), '(import ./. { inherit params inputs; }).api', { build: true, atRev: session.rev })
      setSdk(sdkBuild(result))
    } catch (e) {
      setSdk({ building: false, failure: errorText(e) })
    }
  }

  const canFormat = (p: string) => /[^/]\.(json|nix)$/.test(p)

  const format = async (p: string) => {
    const rpc = session.rpc
    const source = model.loadedText(p)
    if (!rpc || !canFormat(p) || source === undefined || formatting) return
    setFormatting(true)
    try {
      const r = await hostCall<H.FileFormatResultT>(H.RequestPayload.FileFormatRequest,
        new H.FileFormatRequestT(p, source), H.ReplyPayload.FileFormatResult)
      if (!r.diagnostics.length) {
        model.setText(p, s(r.text))
        setFormatMessage(L('整形しました'))
        setFormatMarkers([])
      } else {
        setFormatMarkers(r.diagnostics.map((d) => ({ offset: d.offset ?? 0n, message: s(d.message) })))
        setFormatMessage(L('構文エラーがあります（テキストは変更していません）'))
      }
    } catch (e) {
      setFormatMessage(e instanceof RPCFailure && e.kind === D.FailureKind.unsupported ? e.diagnostic ?? L('この形式の整形には対応していません') : errorText(e))
    }
    setFormatting(false)
  }

  const menu = (e: React.MouseEvent, n: FileNode) => showMenu(e, [
    { label: L('開く'), action: () => model.openFile(n.path) },
    { label: L('名前を変更…'), action: () => setRenaming(n.path) },
    { label: L('削除'), destructive: true, action: () => { model.removeFile(n.path); model.closeFile(n.path) } },
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

  const sdkName = (f: string) => f === SDK_LOG ? L('ビルドログ') : sdk.files?.find((file) => file.id === f)?.name ?? f.split('/').pop() ?? f
  const displayName = (f: string) => (f.startsWith(SDK) ? sdkName(f) : f).split('/').pop() ?? f

  const header = (p: string, generatedFile: boolean) => {
    const rel = generatedFile ? sdkName(p) : p.slice(prefix.length)
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
        {model.created.has(p) ? <span className="caption2" style={{ color: 'var(--orange)' }}>{L('新規')}</span>
          : model.isEdited(p) ? <span className="caption2" style={{ color: 'var(--orange)' }}>{L('未適用')}</span> : null}
        <span className="spacer" />
        {formatMessage && <span className="caption secondary ellipsis">{formatMessage}</span>}
        {!generatedFile && canFormat(p) && (
          <Button small disabled={formatting || model.loadedText(p) === undefined} help={L('ソースコードを整形（{0}）', prefs.label('formatSource'))}
            onClick={() => void format(p)}>{L('整形')}</Button>
        )}
      </div>
    )
  }

  let pane: ReactNode
  if (shown?.startsWith(SDK)) {
    const text = shown === SDK_LOG ? sdk.log : sdk.files?.find((file) => file.id === shown)?.text
    pane = (
      <>
        {header(shown, true)}
        {text !== undefined ? (
          <CodeEditor text={text} language={shown === SDK_LOG ? undefined : shown.split('.').pop()} editable={false} documentId={shown} className="src-editor" />
        ) : (
          <Empty title={L('SDK をビルドすると表示されます')} icon="doc.text" />
        )}
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
          <CodeEditor text={text} language={shown.split('.').pop()} markers={markers} documentId={shown} revealLine={model.sourceLine} className="src-editor"
            onChange={(t) => {
              model.setText(shown, t)
              if (formatMarkers.length) setFormatMarkers([])
            }} />
        ) : load?.state === 'failed' ? (
          // Never an editable empty file: the content is unknown.
          <Empty title={L('ファイルを読み込めませんでした')} icon="exclamationmark.triangle" description={<span className="wrap-pre">{L('内容が不明なため、編集・名前変更・保存はできません。\n{0}', load.error)}</span>}>
            <Button onClick={() => void model.text(shown).catch(() => {})}>{L('再試行')}</Button>
          </Empty>
        ) : (
          <div className="center-fill col gap8"><Spinner /><span className="secondary">{L('読み込んでいます…')}</span></div>
        )}
        {markers.length > 0 && (
          <div className="src-markers">
            {markers.map((m, i) => <span key={i} className="label caption selectable" style={{ color: 'var(--red)' }}><Icon name="xmark.octagon" />{m.message}</span>)}
          </div>
        )}
      </>
    )
  } else {
    pane = <Empty title={L('ファイルを選んでください')} icon="doc.text" />
  }

  const editor = (
    <div key="editor" className="src-pane" onKeyDown={(e) => {
      if (e.nativeEvent.isComposing || !shown || shown.startsWith(SDK) || !canFormat(shown)) return
      if (prefs.action(e.nativeEvent, false) === 'formatSource') {
        e.preventDefault()
        e.stopPropagation()
        void format(shown)
      }
    }}>
      {model.openFiles.length > 1 && (
        <div className="src-tabs" role="tablist">
          {model.openFiles.map((f) => (
            <div key={f} role="tab" aria-selected={shown === f} className={`src-tab${shown === f ? ' shown' : ''}`} title={f} onClick={() => model.openFile(f)}>
              <Icon name={f.startsWith(SDK) ? 'lock.doc' : fileIcon(f)} size={11} />
              <span className="caption ellipsis">{displayName(f)}</span>
              {model.isEdited(f) && <span className="edited-dot" title={L('未適用の変更')} />}
              <button type="button" className="src-tab-close" title={L('タブを閉じる（内容は保持されます）')} aria-label={L('タブを閉じる')}
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
      <HSplit sizes={[220, null]} min={[170, 320]} storageKey="tsugumi.sourceSplit">
          {[
            <div key="files" className="src-files">
              <div className="scroll flex1 src-tree">
                {tree(fileTree(files.map((f) => f.slice(prefix.length)), prefix), 0)}
                {model.kind === 'code' && (
                  <>
                    <div className="src-section caption row center gap4">
                      <span className="spacer">{L('SDK')}</span>
                      <Button small disabled={sdk.building || !session.rpc} onClick={() => void buildSdk()}
                        help={L('保存済みの定義と現在の接続で SDK をビルドし、share/tsugumi/api/ のファイルを表示します')}>
                        {sdk.building ? L('ビルドしています…') : L('SDK をビルド')}
                      </Button>
                    </div>
                    {sdk.failure !== undefined && (
                      <>
                        <div className="caption selectable pad6 wrap-pre" style={{ color: 'var(--red)' }}>{L('ビルドできませんでした: {0}', sdk.failure)}</div>
                        {sdk.log && (
                          <FileRow name={L('ビルドログ')} icon="doc.text" depth={0} selected={shown === SDK_LOG} readOnly onClick={() => model.openFile(SDK_LOG)} />
                        )}
                      </>
                    )}
                    {sdk.truncated && <div className="caption pad6">{L('出力の上限に達したため、一部のファイルのみ表示しています')}</div>}
                    {sdk.files?.length === 0 && (
                      <div className="caption pad6">{L('ビルドで生成された SDK ファイルはありません')}</div>
                    )}
                    {sdk.files?.map((file) => (
                      <FileRow key={file.id} name={file.name} icon="lock.doc" depth={0} selected={shown === file.id} readOnly help={file.name} onClick={() => model.openFile(file.id)} />
                    ))}
                  </>
                )}
              </div>
              <div className="row gap4 pad6 border-top">
                <Button kind="borderless" iconOnly icon="plus" help={L('ファイルを作成')} onClick={() => setShowNew(true)} />
                <Button kind="borderless" iconOnly icon="minus" help={L('表示中のファイルを削除')} disabled={!shown || shown.startsWith(SDK)}
                  onClick={() => { if (shown && !shown.startsWith(SDK)) { model.removeFile(shown); model.closeFile(shown) } }} />
              </div>
            </div>,
            editor,
          ]}
        </HSplit>
      {showNew && <Prompt title={L('ファイルを作成')} initial="src/" placeholder={L('パス（例: src/util.py）')} confirm={L('作成')} onCancel={() => setShowNew(false)}
        onConfirm={(v) => { model.createFile(v); model.openFile(prefix + v) }} />}
      {renaming && <Prompt title={L('名前を変更')} initial={renaming.slice(prefix.length)} placeholder={L('新しいパス')} confirm={L('変更')} onCancel={() => setRenaming(undefined)}
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
