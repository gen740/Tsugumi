// One open project: shared by every view showing it. Owns the RPC
// connection, the confirmed document (from tsugumid), the local draft with
// undo/redo, run progress, displays and logs. Every edit is saved at once:
// the draft holds only what is on its way to the core, or what could not be
// saved (a run in progress, a lost connection).
import { api, EnvPublicState, onServerEvent } from '../api'
import { arr, bool, clone, dbl, get, isObj, J, jeq, JObj, num, numText, obj, real, serialize, str, tryParse } from '../shared/json'
import { cardPath, Change, D, R, RPCClient, RPCError, RPCFailure, s, segs, sOpt, uuid, webSocketTransport } from '../shared/rpc'
import {
  applicableStages, CardGeometry, CardNodeData, mergeStageStatus, NEW_PORT, ParamData, PortData, Pt, Stage, stageOfPhase, stageParts, StageProgress, StageStatus,
  snapToGrid,
} from './canvasKit'
import { arrangeCards } from './arrange'
import { cloneDef, cloneExternal, defEq, extEq, merge3, mergeDefinition, mergeExternal } from './merge'
import { Observable } from './observable'
import { FlowDirection, prefs } from './prefs'
import { cardPermissions } from './permissions'

export type CardKey = string // "a/b"
export const key = (p: string[]): CardKey => p.join('/')
export const path = (k: CardKey): string[] => (k ? k.split('/') : [])

export interface Diagnostic {
  id: string
  error: boolean
  code: string
  message: string
  card?: string[]
  path?: string
  offset?: bigint
}

const diag = (d: D.DiagnosticT): Diagnostic => ({
  id: uuid(), error: d.severity !== D.Severity.warning, code: s(d.code), message: s(d.message),
  card: d.card ? segs(d.card) : undefined, path: sOpt(d.path), offset: d.offset ?? undefined,
})

export interface PortInfo {
  name: string
  kind: D.PortKind
  optional: boolean
  rootType?: string
}

export interface ResolvedCard {
  path: string[]
  inputs: PortInfo[]
  outputs: PortInfo[]
  diagnostics: Diagnostic[]
}

/** One stage of an invocation as the core recorded it (core clock, ms). */
export interface StageRecord {
  stage: Stage
  outcome: D.StageOutcome
  start?: number
  end?: number
}

export const stageRecords = (list: R.StageTimeT[]): StageRecord[] =>
  list.flatMap((t) => {
    const st = stageOfPhase(t.phase)
    return st === undefined ? [] : [{ stage: st, outcome: t.outcome, start: t.start !== null ? Number(t.start) : undefined, end: t.end !== null ? Number(t.end) : undefined }]
  })

/** Latest invocation of a Card in the current run, as the core reports it. */
export interface CardRunState {
  state: D.CardState
  /** Number of the latest invocation in this run (1, 2, …); 0 = not run. */
  invocation: bigint
  phase: D.Phase
  failure?: string
  /** Stages of the latest invocation, in the order the core ran them. */
  stages: StageRecord[]
  cached: boolean
  workerReused: boolean
  /** Outputs the latest invocation sent, and every output that sent a value in this run. */
  sent: Set<string>
  sentInRun: Set<string>
  startedAt?: number
  endedAt?: number
  readyAt?: number
}

export const runStateStage = (st: CardRunState): Stage | undefined =>
  st.stages.length ? st.stages[st.stages.length - 1].stage : st.state === D.CardState.waiting || st.state === D.CardState.ready ? Stage.waiting : undefined

/** One invocation on the run timeline (times in ms since 1970, from the core). */
export interface TimelineEntry {
  card: CardKey
  invocation: bigint
  state: D.CardState
  startedAt?: number
  endedAt?: number
  stages: StageRecord[]
}

/** A Draft being written on its Card: its name and content as typed so far. */
export interface InlineDraft {
  card: CardKey
  name: string
  content: string
}

/** Draft of one Card's definition (`_card.json` and `_meta.json`, or for a
 *  Card in Card `_link.json`), or its removal. */
export interface DefinitionDraft {
  card: J
  meta?: J
  removed?: boolean
  link?: J
}

/** A project inside this one (externals/<name>/, nested further): the inner
 *  canvas of a Card in Card. A copy owned by this project, edited here like
 *  the project's own documents (written back as its files). */
export interface ExternalProject {
  project: J
  layout: J
  /** Card id -> its definition files (_card.json or _link.json, _meta.json). */
  definitions: Map<string, DefinitionDraft>
  /** Removed (the folder goes with it). */
  removed?: boolean
}

/** Card id -> external name (its own Cards in Card). */
export const externalLinks = (e: ExternalProject) =>
  new Map([...e.definitions].flatMap(([id, d]) => {
    const n = str(get(d.link, 'external'))
    return n ? [[id, n] as [string, string]] : []
  }))

/** The documents as files, relative to the external's folder. */
export function externalFiles(e: ExternalProject): Map<string, string> {
  const out = new Map([['tsugumi.json', serialize(e.project)], ['layout.json', serialize(e.layout)]])
  for (const [id, d] of e.definitions) {
    const dir = `definitions/${id}/`
    if (d.link !== undefined) out.set(dir + '_link.json', serialize(d.link))
    else out.set(dir + '_card.json', serialize(d.card))
    if (d.meta !== undefined) out.set(dir + '_meta.json', serialize(d.meta))
  }
  return out
}

/** Whether a path (relative to an external) is one of its documents. */
export function isDocument(rel: string): boolean {
  if (rel === 'tsugumi.json' || rel === 'layout.json') return true
  const parts = rel.split('/')
  return parts.length === 3 && parts[0] === 'definitions' && ['_card.json', '_link.json', '_meta.json'].includes(parts[2])
}

/** Save/sync state, shown separately from the run state. */
export type SyncState =
  | { kind: 'saved' }
  | { kind: 'saving' }
  | { kind: 'failed'; message: string }
  | { kind: 'offline' }
  /** Card changes wait until the run ends. */
  | { kind: 'waitingForRun' }

export interface LineSeries {
  id: string
  label: string
  x: number[]
  y: number[]
}

export type DisplayValue =
  | { kind: 'text'; text: string }
  /** Markdown with TeX math (the SDK's markdown display, a logger in Markdown mode). */
  | { kind: 'markdown'; text: string }
  | { kind: 'scalar'; value: number; label: string }
  | { kind: 'lineChart'; series: LineSeries[] }
  | { kind: 'table'; columns: string[]; rows: string[][] }
  | { kind: 'image'; mediaType: string; data: Uint8Array; alt: string }
  | { kind: 'points'; x: number[]; y: number[]; z: number[]; colors: number[] }
  | { kind: 'module'; name: string; json: string }

export interface DisplaySnapshot {
  runId: string
  invocation: bigint
  revision: bigint
  value: DisplayValue
}

export function displaySummary(d: DisplaySnapshot): string {
  const v = d.value
  switch (v.kind) {
    case 'text': return v.text.split('\n')[0] ?? ''
    case 'markdown': {
      // The first line with text, without Markdown marks.
      const line = v.text.split('\n').find((l) => l.trim()) ?? ''
      return line.replace(/^[#>*\-` ]+|[#>*\-` ]+$/g, '').replace(/\*\*/g, '')
    }
    case 'scalar': return `${v.label} = ${Number(v.value.toPrecision(6))}`
    case 'lineChart': return `折れ線 ${v.series.length} 系列 · ${Math.max(0, ...v.series.map((x) => x.x.length))} 点`
    case 'table': return `表 ${v.columns.length} 列 × ${v.rows.length} 行`
    case 'image': return v.alt || '画像'
    case 'points': return `点群 ${v.x.length} 点`
    case 'module': return plotTitle(v.name, v.json) ?? `モジュール ${v.name}`
  }
}

/** A Plotly figure's or Vega-Lite chart's title (as an image shows its alt
 *  text), or what it is when it has none. */
function plotTitle(name: string, json: string): string | undefined {
  if (name !== 'plotly' && name !== 'vega-lite') return undefined
  let v: any
  try { v = JSON.parse(json) } catch { v = undefined }
  const t = name === 'plotly' ? v?.layout?.title : v?.title
  const text = typeof t === 'string' ? t : typeof t?.text === 'string' ? t.text : ''
  return text || (name === 'plotly' ? 'Plotly のグラフ' : 'Vega-Lite のグラフ')
}

export type EnvironmentState = EnvPublicState | { state: 'reconnecting' }

interface Snapshot {
  project: J
  layout: J
  sources: Map<string, string>
  removals: Set<string>
  definitions: Map<string, DefinitionDraft>
  externals: Map<string, ExternalProject>
}

/** The documents as they look with the draft applied (what an undo entry records). */
interface DocState {
  project: J
  layout: J
  definitions: Map<string, DefinitionDraft>
  externals: Map<string, ExternalProject>
}

/** A source file's content before or after an edit ('confirmed': the
 *  confirmed file, read before the edit is saved, for undo). */
type SourceValue = { kind: 'absent' } | { kind: 'text'; text: string } | { kind: 'confirmed' }
const svEq = (a: SourceValue, b: SourceValue) => a.kind === b.kind && (a.kind !== 'text' || a.text === (b as { text: string }).text)

/** One saved operation: undo applies `before` over what is there now, so
 *  changes made since by the Agent or another window stay. */
interface UndoEntry {
  before: DocState
  after: DocState
  sourcesBefore: Map<string, SourceValue>
  sourcesAfter: Map<string, SourceValue>
}

const mapEq = <V>(a: Map<string, V>, b: Map<string, V>, eq: (x: V, y: V) => boolean) =>
  a.size === b.size && [...a].every(([k, v]) => b.has(k) && eq(v, b.get(k)!))
const setEq = (a: Set<string>, b: Set<string>) => a.size === b.size && [...a].every((x) => b.has(x))
const snapEq = (a: Snapshot, b: Snapshot) =>
  jeq(a.project, b.project) && jeq(a.layout, b.layout) && mapEq(a.sources, b.sources, (x, y) => x === y) &&
  setEq(a.removals, b.removals) && mapEq(a.definitions, b.definitions, defEq) && mapEq(a.externals, b.externals, extEq)

interface DraftBase {
  rev: bigint
  hashes: Map<string, string>
  project: J
  layout: J
  definitions: Map<string, DefinitionDraft>
}

const ACTIVE = [D.RunState.queued, D.RunState.preparing, D.RunState.running, D.RunState.cancelling]
const ENDED = [D.CardState.succeeded, D.CardState.failed, D.CardState.cancelled, D.CardState.skipped, D.CardState.blocked]

export const errorText = (e: unknown): string => {
  if (e instanceof RPCFailure) return `${e.kindName}: ${e.diagnostic ?? ''}`
  return e instanceof Error ? e.message : String(e)
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))
type Notification = Parameters<NonNullable<RPCClient['onNotification']>>[0]

/** What a version's summary is written from: its message and what it changed. */
export function versionSummaryInput(c: Pick<R.VersionCommitT, 'message' | 'changed'>, diff: Pick<R.VersionDiffT, 'text' | 'truncated'>): string {
  const cards = (c.changed ?? []).map((k) => s(k) || 'the Workflow itself').join(', ')
  return `Commit message: ${s(c.message).trim()}\nChanged Cards: ${cards}\n\nDiff${diff.truncated ? ' (cut short)' : ''}:\n${s(diff.text)}`
}

export class ProjectSession extends Observable {
  readonly viewId: string
  readonly origin: string

  // Connection
  environment: EnvironmentState = { state: 'stopped' }
  connected = false
  connectionMessage?: string
  private client?: RPCClient
  private subscription?: string
  private lastSeq = 0n
  projectId?: string
  endpoint?: string
  readyFile?: string
  private reconnecting = false

  // Confirmed document (owned by core)
  rev = 0n
  executionRev = 0n
  confirmedProject: J = obj()
  confirmedLayout: J = obj()
  confirmedDefinitions = new Map<string, J>()
  confirmedDefinitionMeta = new Map<string, J>()
  confirmedLinks = new Map<string, J>()
  /** Externals by folder ("externals/<name>/", nested "…/externals/<name>/"). */
  externals = new Map<string, ExternalProject>()
  private externalsKey = ''
  sources = new Map<string, { hash: string; size: bigint }>()
  resolution = new Map<CardKey, ResolvedCard>()
  diagnostics: Diagnostic[] = []

  // Draft
  project: J = obj()
  layout: J = obj()
  sourceEdits = new Map<string, string>() // path -> new text
  sourceRemovals = new Set<string>()
  definitionEdits = new Map<string, DefinitionDraft>()
  /** The Draft being written on the canvas (geometry opens its Card up). */
  inlineDraft?: InlineDraft
  /** Edited externals (by folder), written back as their files. */
  externalEdits = new Map<string, ExternalProject>()
  /** Base hashes chosen by an editor that read the file before this draft started. */
  sourceBases = new Map<string, string>()
  undoStack: UndoEntry[] = []
  redoStack: UndoEntry[] = []
  saveError?: string
  saving = false
  /** Cards renamed inside Cards in Card by edits not saved yet. */
  pendingCardRenames: { from: string[]; to: string[] }[] = []
  /** Edits wait for the run to end (the core keeps Card changes back while it runs). */
  savingAfterRun = false
  /** Last user-facing problem of an action (run refused, import failed, ...). */
  notice?: string

  private draftBase?: DraftBase
  /** Confirmed externals when each external's edit started. */
  private externalBases = new Map<string, ExternalProject | null>()
  /** Hashes of the externals' documents as they were read into `externals`. */
  private externalHashes = new Map<string, string>()
  private saveTask?: Promise<void>
  private saveAgain = false
  private conflictRetries = 0
  private unfilled: UndoEntry[] = []

  // View state saved with ViewSave: the active tab's scope and selection.
  initialScope: string[] = []
  initialSelection = new Set<CardKey>()
  cameras = new Map<CardKey, { x: number; y: number; zoom: number }>()
  private viewRev = 0n
  private viewSaveTimer?: ReturnType<typeof setTimeout>

  // Runs
  runStatus?: R.RunStatusT
  cardStates = new Map<CardKey, CardRunState>()
  displays = new Map<CardKey, DisplaySnapshot>()
  lastFinished = 0
  timeline: TimelineEntry[] = []
  /** Browser clock minus core clock (ms): the smallest difference seen. */
  coreClockOffset?: number
  logRevision = 0 // bumps when new log bytes are available

  /** How many notifications the UI has taken in, and in how many batches. */
  notificationCount = 0
  notificationBatches = 0
  private inbox: Notification[] = []
  private inboxScheduled = false
  private offEvents: () => void

  constructor(readonly dir: string, readonly key: string, readonly folderName: string) {
    super()
    // A stable view id per project and browser keeps camera/selection across launches.
    const k = `tsugumi.view.${key}`
    let v: string | null = null
    try {
      v = localStorage.getItem(k)
    } catch {}
    if (!v) {
      v = uuid()
      try {
        localStorage.setItem(k, v)
      } catch {}
    }
    this.viewId = v
    this.origin = `ui:${v}`
    this.offEvents = onServerEvent((e) => {
      if (e.type === 'reconnected' && !this.connected) {
        // The environment's progress may have been missed while the stream was down.
        void api<{ key: string; state: EnvPublicState }[]>('/api/env/list').then((list) => {
          const mine = list.find((x) => x.key === this.key)
          if (mine && !this.connected) {
            this.environment = mine.state
            this.changed()
          }
        }).catch(() => {})
      }
      if (e.type === 'env' && e.key === this.key && !this.connected) {
        this.environment = e.state
        this.changed()
      }
    })
  }

  /** The core clock now (ms since 1970), for elapsed times of a running run. */
  coreNow(): number {
    return Date.now() - (this.coreClockOffset ?? 0)
  }

  private sampleClock(coreMs: bigint | number | null | undefined) {
    if (coreMs === null || coreMs === undefined) return
    const d = Date.now() - Number(coreMs)
    if (this.coreClockOffset === undefined || d < this.coreClockOffset) this.coreClockOffset = d
  }

  get draftBaseRev(): bigint {
    return this.draftBase?.rev ?? this.rev
  }

  get dirty(): boolean {
    return !jeq(this.layout, this.confirmedLayout) || this.computationDirty
  }

  get computationDirty(): boolean {
    return !jeq(this.project, this.confirmedProject) || this.sourceEdits.size > 0 || this.sourceRemovals.size > 0 || this.definitionEdits.size > 0 ||
      this.externalEdits.size > 0
  }

  get syncState(): SyncState {
    if (!this.connected) return this.dirty ? { kind: 'failed', message: this.connectionMessage ?? '接続していません' } : { kind: 'offline' }
    if (this.saving) return { kind: 'saving' }
    if (this.saveError) return { kind: 'failed', message: this.saveError }
    if (this.savingAfterRun && this.dirty) return { kind: 'waitingForRun' }
    return this.dirty ? { kind: 'saving' } : { kind: 'saved' }
  }

  /** Effective definitions: confirmed ones with the draft applied. */
  get definitions(): Map<string, J> {
    const d = new Map(this.confirmedDefinitions)
    for (const [id, e] of this.definitionEdits) {
      if (e.removed || e.link !== undefined) d.delete(id)
      else d.set(id, e.card)
    }
    return d
  }

  /** Card in Card links of this project's Cards (_link.json), with the draft applied. */
  get links(): Map<string, J> {
    const d = new Map(this.confirmedLinks)
    for (const [id, e] of this.definitionEdits) {
      if (e.removed || e.link === undefined) d.delete(id)
      else d.set(id, e.link)
    }
    return d
  }

  get definitionMeta(): Map<string, J> {
    const d = new Map(this.confirmedDefinitionMeta)
    for (const [id, e] of this.definitionEdits) {
      if (e.removed || e.meta === undefined) d.delete(id)
      else d.set(id, e.meta)
    }
    return d
  }

  hasSource(p: string): boolean {
    return !this.sourceRemovals.has(p) && (this.sources.has(p) || this.sourceEdits.has(p))
  }

  /** Source paths of the draft (confirmed, plus new files, minus removed ones). */
  get sourcePaths(): string[] {
    const all = new Set([...this.sources.keys(), ...this.sourceEdits.keys()])
    for (const p of this.sourceRemovals) all.delete(p)
    return [...all].sort()
  }

  get runActive(): boolean {
    const st = this.runStatus?.state
    return st !== undefined && ACTIVE.includes(st)
  }

  get currentRunId(): string | undefined {
    return sOpt(this.runStatus?.runId)
  }

  get rpc(): RPCClient | undefined {
    return this.client
  }

  get isEditable(): boolean {
    return this.connected && !this.runActive
  }

  // ---- Connection ------------------------------------------------------------------

  start() {
    // Shown as starting at once: the loading card, never a "stopped" page.
    if (this.environment.state === 'stopped') this.environment = { state: 'starting', message: '' }
    void this.openConnection()
  }

  private async openConnection() {
    this.connectionMessage = undefined
    this.changed()
    try {
      const env = await api<EnvPublicState>('/api/env/start', { path: this.dir })
      this.environment = env
      this.changed()
      if (env.state !== 'ready') return
      const c = new RPCClient(await webSocketTransport(env.endpoint!))
      await c.connect(env.token)
      const opened = await c.open(this.viewId)
      this.client = c
      this.projectId = env.projectId
      this.endpoint = env.endpoint
      this.readyFile = env.readyFile
      this.apply(opened.snapshot!, !this.dirty)
      this.subscription = s(opened.subscriptionId)
      this.lastSeq = opened.seq ?? 1n
      this.connected = true
      this.listen(c)
      c.onClose = () => {
        if (this.client !== c) return
        this.connected = false
        this.changed()
        this.reconnect()
      }
      if (this.makeRepositoryOnConnect) {
        this.makeRepositoryOnConnect = false
        await c.versionInit(uuid(), 'ui').catch(() => {})
      }
      if (this.dirty) this.scheduleSave()
      this.changed()
      await this.refreshVisibleDisplays()
    } catch (e) {
      this.connected = false
      this.connectionMessage = errorText(e)
      if (this.environment.state === 'starting' || this.environment.state === 'reconnecting') this.environment = { state: 'ready' }
      this.changed()
    }
  }

  /** Reconnects after a lost connection: Hello -> Open -> re-read state. */
  reconnect() {
    if (this.reconnecting) return
    this.reconnecting = true
    this.environment = { state: 'reconnecting' }
    const old = this.client
    this.client = undefined
    old?.close()
    this.connected = false
    this.changed()
    void (async () => {
      for (let attempt = 0; attempt < 20; attempt++) {
        await this.openConnection()
        if (this.connected) break
        await sleep(Math.min(5000, 250 * (attempt + 1)))
      }
      this.reconnecting = false
    })()
  }

  async restartEnvironment() {
    const c = this.client
    this.client = undefined
    c?.close()
    this.connected = false
    this.changed()
    await api('/api/env/stop', { path: this.dir }).catch(() => {})
    await this.openConnection()
  }

  async stopEnvironment() {
    const c = this.client
    this.client = undefined
    c?.close()
    this.connected = false
    this.changed()
    await api('/api/env/stop', { path: this.dir }).catch(() => {})
    this.environment = { state: 'stopped' }
    this.changed()
  }

  dispose() {
    this.offEvents()
    this.client?.close()
  }

  /** Notifications are taken in as they arrive and applied in batches at most
   *  once per frame: a run with thousands of short invocations causes one
   *  view update per batch, not per event. */
  private listen(c: RPCClient) {
    c.onNotification = (e) => {
      this.inbox.push(e)
      if (this.inboxScheduled) return
      this.inboxScheduled = true
      requestAnimationFrame(() => {
        this.inboxScheduled = false
        const batch = this.inbox
        this.inbox = []
        if (batch.length) this.notificationBatches++
        for (const ev of batch) this.handle(ev)
        this.changed()
      })
    }
  }

  private handle(e: Notification) {
    this.notificationCount++
    if (e.subscriptionId !== this.subscription) return // stale subscription
    if (e.seq !== this.lastSeq + 1n) {
      // A gap means we cannot trust our state: open again.
      this.reconnect()
      return
    }
    this.lastSeq = e.seq
    switch (e.type) {
      case R.NotificationPayload.DocPatched: {
        const d = (e.value as R.DocPatchedT).delta
        if (d) this.applyDelta(d)
        break
      }
      case R.NotificationPayload.RunEvent:
        this.applyRunEvent(e.value as R.RunEventT)
        break
      case R.NotificationPayload.DisplayUpdated: {
        const d = (e.value as R.DisplayUpdatedT).display
        if (d) this.applyDisplay(d)
        break
      }
      case R.NotificationPayload.LogAvailable:
        this.logRevision++
        break
    }
  }

  // ---- Applying core state -----------------------------------------------------------

  private readDefinitions(defs: R.DefinitionT[]) {
    for (const d of defs) {
      const id = s(d.id)
      if (d.linkJson !== null) {
        const l = tryParse(s(d.linkJson))
        if (l !== undefined) this.confirmedLinks.set(id, l)
        this.confirmedDefinitions.delete(id)
      } else {
        const c = tryParse(s(d.cardJson))
        if (c !== undefined) this.confirmedDefinitions.set(id, c)
        this.confirmedLinks.delete(id)
      }
      const m = d.metaJson !== null ? tryParse(s(d.metaJson)) : undefined
      if (m !== undefined) this.confirmedDefinitionMeta.set(id, m)
      else this.confirmedDefinitionMeta.delete(id)
    }
  }

  private apply(snap: R.ProjectSnapshotT, resetDraft: boolean) {
    this.rev = snap.rev ?? 0n
    this.executionRev = snap.executionRev ?? 0n
    this.confirmedProject = tryParse(s(snap.projectJson)) ?? obj()
    this.confirmedLayout = tryParse(s(snap.layoutJson)) ?? obj()
    this.confirmedDefinitions = new Map()
    this.confirmedDefinitionMeta = new Map()
    this.confirmedLinks = new Map()
    this.readDefinitions(snap.definitions)
    this.sources = new Map()
    for (const src of snap.sources) this.sources.set(s(src.path), { hash: s(src.hash), size: src.sizeBytes ?? 0n })
    this.refreshExternals()
    this.applyResolution(snap.resolution, snap.diagnostics)
    this.runStatus = snap.run ?? undefined
    this.viewRev = snap.viewRev ?? 0n
    if (snap.view) {
      this.initialScope = segs(snap.view.scope)
      for (const cam of snap.view.cameras) this.cameras.set(key(segs(cam.scope)), { x: cam.x ?? 0, y: cam.y ?? 0, zoom: cam.zoom ?? 1 })
      this.initialSelection = new Set(snap.view.selectedCards.map((p) => key(segs(p))))
    }
    if (resetDraft) {
      this.project = clone(this.confirmedProject)
      this.layout = clone(this.confirmedLayout)
      this.sourceEdits = new Map()
      this.sourceRemovals = new Set()
      this.definitionEdits = new Map()
      this.externalEdits = new Map()
      this.draftBase = undefined
      this.externalBases = new Map()
    } else if (this.draftBase?.rev !== this.rev) {
      // Edits kept over a reconnect: merged onto what is there now, then saved.
      this.rebase()
    }
  }

  private applyResolution(res: R.ResolvedCardT[], diags: D.DiagnosticT[]) {
    const r = new Map<CardKey, ResolvedCard>()
    const all = diags.map(diag)
    const port = (x: D.PortT): PortInfo => ({ name: s(x.name), kind: x.kind, optional: !!x.optional, rootType: sOpt(x.rootType) })
    for (const rc of res) {
      const p = segs(rc.card)
      const d = rc.diagnostics.map(diag)
      r.set(key(p), { path: p, inputs: rc.inputs.map(port), outputs: rc.outputs.map(port), diagnostics: d })
      all.push(...d)
    }
    this.resolution = r
    this.diagnostics = all
    // A result belongs to a Card of the document. A Card that comes back asks for
    // its own again: displayedCards changed (refreshVisibleDisplays).
    for (const k of [...this.displays.keys()]) if (!r.has(k)) this.displays.delete(k)
  }

  private applyDelta(d: R.DocumentDeltaT, fromSave = false) {
    if (d.baseRev !== this.rev) {
      if (d.rev !== null && d.rev <= this.rev) return // already applied (our own save reply)
      this.reconnect()
      return
    }
    const wasClean = !this.dirty
    this.rev = d.rev ?? this.rev
    this.executionRev = d.executionRev ?? this.executionRev
    if (d.projectJson !== null) {
      const v = tryParse(s(d.projectJson))
      if (v !== undefined) this.confirmedProject = v
    }
    if (d.layoutJson !== null) {
      const v = tryParse(s(d.layoutJson))
      if (v !== undefined) this.confirmedLayout = v
    }
    for (const id of d.definitionsRemoved) {
      this.confirmedDefinitions.delete(s(id))
      this.confirmedDefinitionMeta.delete(s(id))
      this.confirmedLinks.delete(s(id))
    }
    this.readDefinitions(d.definitionsUpsert)
    for (const src of d.sourcesUpsert) this.sources.set(s(src.path), { hash: s(src.hash), size: src.sizeBytes ?? 0n })
    for (const p of d.sourcesRemoved) this.sources.delete(s(p))
    this.refreshExternals()
    const shown = [...this.displayedCards].sort().join('|')
    this.applyResolution(d.resolution, d.diagnostics)
    // A Draft implemented (or any Card that became code or a logger) sends its displays too.
    if ([...this.displayedCards].sort().join('|') !== shown) void this.refreshVisibleDisplays()
    if (wasClean) {
      // Nothing local to keep: follow the confirmed document.
      this.project = clone(this.confirmedProject)
      this.layout = clone(this.confirmedLayout)
      this.draftBase = undefined
    } else if (!fromSave) {
      // Changed elsewhere (the Agent, another window) while edits are on
      // their way: shown at once, the edits merged on top.
      this.rebase()
    }
  }

  private applyRunEvent(e: R.RunEventT) {
    switch (e.valueType) {
      case R.RunEventValue.RunAcceptedEvent: {
        const st = (e.value as R.RunAcceptedEventT).status
        if (st) {
          this.runStatus = st
          this.cardStates = new Map()
          this.timeline = []
          this.sampleClock(st.acceptedAt)
        }
        break
      }
      case R.RunEventValue.RunStatusChanged: {
        const st = (e.value as R.RunStatusChangedT).status
        if (st && (!this.runStatus || s(st.runId) === s(this.runStatus.runId))) this.runStatus = st
        if (st?.commit && (st.state === D.RunState.succeeded || st.state === D.RunState.failed || st.state === D.RunState.cancelled)) void this.summarizeVersion(s(st.commit))
        break
      }
      case R.RunEventValue.RunCardChanged: {
        const inv = (e.value as R.RunCardChangedT).invocation
        if (!inv) break
        const k = key(segs(inv.card))
        const prev = this.cardStates.get(k)
        const n = inv.invocation ?? 0n
        if (!prev || prev.invocation <= n) {
          const sent = new Set(inv.outputsSent.map((x) => s(x)))
          this.cardStates.set(k, {
            state: inv.state, invocation: n, phase: inv.phase, failure: sOpt(inv.failure?.diagnostic),
            // Each notification carries the invocation's stages so far.
            stages: stageRecords(inv.stages), cached: !!inv.buildCached, workerReused: !!inv.workerReused,
            sent, sentInRun: new Set([...(prev?.sentInRun ?? []), ...sent]),
            startedAt: inv.startedAt !== null ? Number(inv.startedAt) : undefined, endedAt: inv.endedAt !== null ? Number(inv.endedAt) : undefined,
            readyAt: inv.readyAt !== null ? Number(inv.readyAt) : undefined,
          })
        }
        const lastStart = inv.stages.length ? inv.stages[inv.stages.length - 1].start : null
        this.sampleClock(inv.endedAt ?? (inv.state === D.CardState.running ? lastStart : null))
        this.recordTimeline(k, inv)
        break
      }
      case R.RunEventValue.RunFinished: {
        const st = (e.value as R.RunFinishedT).summary?.status
        if (st) this.runStatus = st
        this.lastFinished = Date.now()
        if (this.dirty) this.scheduleSave()
        break
      }
    }
  }

  private applyDisplay(d: D.DisplayT) {
    const k = key(segs(d.card))
    const rev = d.revision ?? 0n
    const old = this.displays.get(k)
    if (old && old.runId === s(d.runId) && old.revision >= rev) return // stale frame
    let value: DisplayValue
    switch (d.valueType) {
      case D.DisplayValue.DisplayText: {
        const t = d.value as D.DisplayTextT
        value = t.markdown ? { kind: 'markdown', text: s(t.text) } : { kind: 'text', text: s(t.text) }
        break
      }
      case D.DisplayValue.DisplayScalar: {
        const v = d.value as D.DisplayScalarT
        value = { kind: 'scalar', value: v.value ?? 0, label: s(v.label) }
        break
      }
      case D.DisplayValue.DisplayLineChart:
        value = { kind: 'lineChart', series: (d.value as D.DisplayLineChartT).series.map((x) => ({ id: s(x.id), label: s(x.label), x: x.x, y: x.y })) }
        break
      case D.DisplayValue.DisplayTable: {
        const t = d.value as D.DisplayTableT
        value = { kind: 'table', columns: t.columns.map((c) => s(c)), rows: t.rows.map((r) => r.cells.map((c) => s(c))) }
        break
      }
      case D.DisplayValue.DisplayImage: {
        const im = d.value as D.DisplayImageT
        value = { kind: 'image', mediaType: s(im.mediaType), data: Uint8Array.from(im.data), alt: s(im.alt) }
        break
      }
      case D.DisplayValue.DisplayPointCloud: {
        const p = d.value as D.DisplayPointCloudT
        value = { kind: 'points', x: p.x, y: p.y, z: p.z, colors: p.colors }
        break
      }
      case D.DisplayValue.DisplayModule: {
        const m = d.value as D.DisplayModuleT
        value = { kind: 'module', name: s(m.name), json: s(m.dataJson) }
        break
      }
      default:
        return
    }
    this.displays.set(k, { runId: s(d.runId), invocation: d.invocation ?? 0n, revision: rev, value })
  }

  private recordTimeline(k: CardKey, inv: R.InvocationSummaryT) {
    const n = inv.invocation ?? 0n
    const stages = stageRecords(inv.stages)
    const st = inv.startedAt !== null ? Number(inv.startedAt) : undefined
    const en = inv.endedAt !== null ? Number(inv.endedAt) : undefined
    const i = this.timeline.findIndex((t) => t.card === k && t.invocation === n)
    if (i >= 0) {
      const e = this.timeline[i]
      this.timeline[i] = { ...e, state: inv.state, startedAt: st ?? e.startedAt, endedAt: en ?? e.endedAt, stages: stages.length ? stages : e.stages }
      this.timeline = [...this.timeline]
    } else {
      this.timeline = [...this.timeline, { card: k, invocation: n, state: inv.state, startedAt: st, endedAt: en, stages }]
    }
  }

  // ---- Editing (draft, undo/redo) ------------------------------------------------------

  private snapshot(): Snapshot {
    return {
      project: clone(this.project), layout: clone(this.layout), sources: new Map(this.sourceEdits), removals: new Set(this.sourceRemovals),
      definitions: new Map([...this.definitionEdits].map(([k, v]) => [k, cloneDef(v)])),
      externals: new Map([...this.externalEdits].map(([k, v]) => [k, cloneExternal(v)])),
    }
  }

  /** Confirmed definitions as definition files (card, meta, link). */
  get confirmedDefinitionDrafts(): Map<string, DefinitionDraft> {
    const d = new Map<string, DefinitionDraft>()
    for (const [id, c] of this.confirmedDefinitions) d.set(id, { card: c, meta: this.confirmedDefinitionMeta.get(id) })
    for (const [id, l] of this.confirmedLinks) d.set(id, { card: obj(), meta: this.confirmedDefinitionMeta.get(id), link: l })
    return d
  }

  /** Definitions with the draft applied, as definition files. */
  get definitionDrafts(): Map<string, DefinitionDraft> {
    const d = this.confirmedDefinitionDrafts
    for (const [id, e] of this.definitionEdits) {
      if (e.removed) d.delete(id)
      else d.set(id, e)
    }
    return d
  }

  /** Externals with the draft applied. */
  get effectiveExternals(): Map<string, ExternalProject> {
    const d = new Map(this.externals)
    for (const [p, e] of this.externalEdits) {
      if (e.removed) d.delete(p)
      else d.set(p, e)
    }
    return d
  }

  private docState(): DocState {
    return {
      project: clone(this.project), layout: clone(this.layout),
      definitions: new Map([...this.definitionDrafts].map(([k, v]) => [k, cloneDef(v)])),
      externals: new Map([...this.effectiveExternals].map(([k, v]) => [k, cloneExternal(v)])),
    }
  }

  private makeBase(): DraftBase {
    return {
      rev: this.rev, hashes: new Map([...this.sources].map(([p, v]) => [p, v.hash])),
      project: this.confirmedProject, layout: this.confirmedLayout, definitions: this.confirmedDefinitionDrafts,
    }
  }

  /** Fixes the draft's base when it leaves the clean state, and forgets it
   *  when the draft becomes clean again. */
  private settleDraftBase() {
    if (!this.dirty) {
      this.draftBase = undefined
      this.externalBases = new Map()
    } else if (!this.draftBase) {
      this.draftBase = this.makeBase()
    }
  }

  /** Sets a definition as it should look with the draft applied. */
  setEffectiveDefinition(id: string, d: DefinitionDraft | undefined) {
    const confirmed = this.confirmedDefinitionDrafts.get(id)
    if (defEq(d, confirmed)) this.definitionEdits.delete(id)
    else this.definitionEdits.set(id, d ?? { card: obj(), removed: true })
  }

  /** Changes an external's documents in the draft. */
  updateExternal(prefix: string, fn: (e: ExternalProject) => void) {
    if (!prefix) return
    const cur = this.externalEdits.get(prefix) ?? this.externals.get(prefix)
    if (!cur) return
    if (!this.externalEdits.has(prefix) && !this.externalBases.has(prefix)) this.externalBases.set(prefix, this.externals.get(prefix) ?? null)
    const e = cloneExternal(cur)
    fn(e)
    this.externalEdits.set(prefix, e)
  }

  /** Adds a new external (its documents; other files go to the sources). */
  createExternal(prefix: string, e: ExternalProject) {
    if (!this.externalBases.has(prefix)) this.externalBases.set(prefix, this.externals.get(prefix) ?? null)
    this.externalEdits.set(prefix, e)
  }

  /** Removes an external with every file in its folder. */
  removeExternal(prefix: string) {
    const cur = this.externalEdits.get(prefix) ?? this.externals.get(prefix)
    if (!cur) return
    if (!this.externalBases.has(prefix)) this.externalBases.set(prefix, this.externals.get(prefix) ?? null)
    this.externalEdits.set(prefix, { ...cloneExternal(cur), removed: true })
    for (const p of this.sourcePaths) if (p.startsWith(prefix)) this.removeSource(p)
  }

  /** Drops edits of externals that equal what is confirmed. */
  private normalizeExternalEdits() {
    for (const [p, e] of [...this.externalEdits]) {
      if ((e.removed && !this.externals.has(p)) || (!e.removed && extEq(e, this.externals.get(p)))) {
        this.externalEdits.delete(p)
        this.externalBases.delete(p)
      }
    }
  }

  private sourceValue(snap: Snapshot, p: string): SourceValue {
    if (snap.removals.has(p)) return { kind: 'absent' }
    const t = snap.sources.get(p)
    if (t !== undefined) return { kind: 'text', text: t }
    return this.sources.has(p) ? { kind: 'confirmed' } : { kind: 'absent' }
  }

  private record(before: DocState, beforeSnap: Snapshot, after: DocState, afterSnap: Snapshot): UndoEntry {
    const paths = new Set([...beforeSnap.sources.keys(), ...afterSnap.sources.keys(), ...beforeSnap.removals, ...afterSnap.removals])
    const sb = new Map<string, SourceValue>(), sa = new Map<string, SourceValue>()
    for (const p of paths) {
      const b = this.sourceValue(beforeSnap, p), a = this.sourceValue(afterSnap, p)
      if (!svEq(b, a)) {
        sb.set(p, b)
        sa.set(p, a)
      }
    }
    const e: UndoEntry = { before, after, sourcesBefore: sb, sourcesAfter: sa }
    if ([...sb.values()].some((v) => v.kind === 'confirmed')) this.unfilled.push(e)
    return e
  }

  /** Applies an edit and saves it at once. One call is one step of undo. */
  edit(change: (s: this) => void) {
    const before = this.docState(), beforeSnap = this.snapshot()
    if (!this.dirty) this.draftBase = this.makeBase()
    change(this)
    this.normalizeExternalEdits()
    const afterSnap = this.snapshot()
    if (snapEq(beforeSnap, afterSnap)) {
      this.settleDraftBase()
      this.changed()
      return
    }
    this.undoStack.push(this.record(before, beforeSnap, this.docState(), afterSnap))
    if (this.undoStack.length > 500) this.undoStack.shift()
    this.redoStack = []
    this.settleDraftBase()
    this.changed()
    this.scheduleSave()
  }

  undo() {
    const e = this.undoStack.pop()
    if (!e) return
    const inverse = this.revert(e)
    if (inverse) this.redoStack.push(inverse)
    this.changed()
  }

  redo() {
    const e = this.redoStack.pop()
    if (!e) return
    const inverse = this.revert(e)
    if (inverse) this.undoStack.push(inverse)
    this.changed()
  }

  /** Puts back what an entry changed (its `before`), over the current state.
   *  Returns the entry that does it again. */
  private revert(e: UndoEntry): UndoEntry | undefined {
    const cur = this.docState(), curSnap = this.snapshot()
    if (!this.dirty) this.draftBase = this.makeBase()
    this.project = clone(merge3(e.after.project, e.before.project, cur.project) ?? cur.project)
    this.layout = clone(merge3(e.after.layout, e.before.layout, cur.layout) ?? cur.layout)
    for (const id of new Set([...e.before.definitions.keys(), ...e.after.definitions.keys()])) {
      if (defEq(e.before.definitions.get(id), e.after.definitions.get(id))) continue
      this.setEffectiveDefinition(id, mergeDefinition(e.after.definitions.get(id), e.before.definitions.get(id), cur.definitions.get(id)))
    }
    for (const p of new Set([...e.before.externals.keys(), ...e.after.externals.keys()])) {
      if (extEq(e.before.externals.get(p), e.after.externals.get(p))) continue
      const b = e.before.externals.get(p)
      if (b) {
        if (!this.externalBases.has(p)) this.externalBases.set(p, this.externals.get(p) ?? null)
        const t = mergeExternal(e.after.externals.get(p), b, cur.externals.get(p))
        t.removed = false
        this.externalEdits.set(p, t)
      } else {
        this.removeExternal(p)
      }
    }
    for (const [p, v] of e.sourcesBefore) {
      if (v.kind === 'absent') this.removeSource(p)
      else if (v.kind === 'text') {
        this.sourceRemovals.delete(p)
        this.sourceEdits.set(p, v.text)
      } // 'confirmed': could not be read: left as it is
    }
    this.normalizeExternalEdits()
    this.settleDraftBase()
    if (snapEq(this.snapshot(), curSnap)) return undefined
    const sb = new Map<string, SourceValue>()
    for (const p of e.sourcesBefore.keys()) sb.set(p, this.sourceValue(curSnap, p))
    const inverse: UndoEntry = { before: cur, after: this.docState(), sourcesBefore: sb, sourcesAfter: e.sourcesBefore }
    if ([...sb.values()].some((v) => v.kind === 'confirmed')) this.unfilled.push(inverse)
    this.scheduleSave()
    return inverse
  }

  /** Reads the files undo entries need before a save replaces them. */
  private async fillUndoSources() {
    const client = this.client
    if (!client || !this.unfilled.length) return
    const list = this.unfilled
    this.unfilled = []
    for (const e of list) {
      for (const [p, v] of e.sourcesBefore) {
        if (v.kind !== 'confirmed') continue
        try {
          e.sourcesBefore.set(p, { kind: 'text', text: new TextDecoder().decode(await client.readFile(p, this.rev)) })
        } catch {}
      }
    }
  }

  /** Moves the draft onto the confirmed state that just arrived: what the
   *  draft changed stays, everything else follows the new state. */
  private rebase() {
    const b = this.draftBase
    if (!b) return
    this.project = clone(merge3(b.project, this.project, this.confirmedProject) ?? this.confirmedProject)
    this.layout = clone(merge3(b.layout, this.layout, this.confirmedLayout) ?? this.confirmedLayout)
    const theirs = this.confirmedDefinitionDrafts
    for (const [id, e] of [...this.definitionEdits]) {
      this.setEffectiveDefinition(id, mergeDefinition(b.definitions.get(id), e.removed ? undefined : e, theirs.get(id)))
    }
    const hashes = new Map([...this.sources].map(([p, v]) => [p, v.hash]))
    // Files edited here stay checked against the version they were edited from.
    for (const p of new Set([...this.sourceEdits.keys(), ...this.sourceRemovals])) {
      const h = b.hashes.get(p)
      if (h === undefined) hashes.delete(p)
      else hashes.set(p, h)
    }
    this.draftBase = { rev: this.rev, hashes, project: this.confirmedProject, layout: this.confirmedLayout, definitions: theirs }
    this.settleDraftBase()
  }

  /** Drops every unsaved edit and follows the latest confirmed document
   *  (only for edits that cannot be saved). */
  discardDraft() {
    this.project = clone(this.confirmedProject)
    this.layout = clone(this.confirmedLayout)
    this.sourceEdits = new Map()
    this.sourceRemovals = new Set()
    this.definitionEdits = new Map()
    this.externalEdits = new Map()
    this.sourceBases = new Map()
    this.draftBase = undefined
    this.externalBases = new Map()
    this.saveError = undefined
    this.savingAfterRun = false
    this.changed()
  }

  /** The unsaved edits as text, for copying them out when they cannot be saved. */
  draftSummary(): string {
    let out = `Tsugumi unsaved edits (based on revision ${this.draftBaseRev})\n`
    if (!jeq(this.project, this.confirmedProject)) out += '\n== tsugumi.json ==\n' + serialize(this.project) + '\n'
    if (!jeq(this.layout, this.confirmedLayout)) out += '\n== layout.json ==\n' + serialize(this.layout) + '\n'
    for (const [id, d] of [...this.definitionEdits].sort(([a], [b]) => a.localeCompare(b))) {
      if (d.removed) {
        out += `\n== definitions/${id} removed ==\n`
        continue
      }
      if (d.link !== undefined) out += `\n== definitions/${id}/_link.json ==\n` + serialize(d.link) + '\n'
      else out += `\n== definitions/${id}/_card.json ==\n` + serialize(d.card) + '\n'
      if (d.meta !== undefined) out += `\n== definitions/${id}/_meta.json ==\n` + serialize(d.meta) + '\n'
    }
    for (const [p, e] of this.externalEdits) {
      if (e.removed) out += `\n== ${p} removed ==\n`
      else for (const [rel, text] of externalFiles(e)) out += `\n== ${p}${rel} ==\n${text}\n`
    }
    for (const [p, t] of [...this.sourceEdits].sort(([a], [b]) => a.localeCompare(b))) out += `\n== ${p} ==\n${t}\n`
    for (const p of [...this.sourceRemovals].sort()) out += `\n== ${p} removed ==\n`
    return out
  }

  /** Starts saving what the draft holds (edits made meanwhile are saved next). */
  scheduleSave() {
    if (!this.connected || !this.client) return
    if (this.saveTask) {
      this.saveAgain = true
      return
    }
    this.saveTask = (async () => {
      let attempts = 0
      try {
        do {
          this.saveAgain = false
          attempts++
          await this.saveOnce()
        } while (this.saveAgain && attempts < 50)
      } catch (e) {
        // An unexpected error ends this save (the edits stay in the draft),
        // never every later one.
        this.saveError = errorText(e)
      } finally {
        this.saveTask = undefined
        this.changed()
      }
    })()
  }

  /** Waits until what was edited so far is saved (or cannot be saved now). */
  async save(): Promise<void> {
    if (!this.saveTask) this.scheduleSave()
    else this.saveAgain = true
    while (this.saveTask) await this.saveTask
  }

  /** Sends the draft as one ProjectSave transaction. Changes that arrived
   *  meanwhile are merged in first; a conflict means newer changes are on
   *  their way, so the save is tried again once they arrived. */
  private async saveOnce(): Promise<void> {
    const client = this.client
    if (!this.dirty || !client) return
    await this.fillUndoSources()
    if (this.draftBase && this.draftBase.rev !== this.rev) this.rebase()
    if (!this.dirty) return
    this.saving = true
    this.changed()
    try {
      const base = this.draftBase ?? this.makeBase()
      // While a run is active the core takes layout changes only.
      const layoutOnly = this.runActive && this.computationDirty
      this.savingAfterRun = layoutOnly
      const changes: R.DocumentChangeT[] = []
      const sent = this.snapshot()
      if (!jeq(this.layout, this.confirmedLayout)) changes.push(Change.setLayout(serialize(this.layout)))
      const merged = new Map<string, ExternalProject>()
      if (!layoutOnly) {
        if (!jeq(this.project, this.confirmedProject)) changes.push(Change.setProject(serialize(this.project)))
        // A removed definition takes its folder with it: the core refuses a change to a file in it.
        const removedFolders: string[] = []
        for (const [id, d] of [...this.definitionEdits].sort(([a], [b]) => a.localeCompare(b))) {
          if (d.removed) {
            if (this.confirmedDefinitions.has(id) || this.confirmedLinks.has(id)) {
              changes.push(Change.removeDefinition(id))
              removedFolders.push(`definitions/${id}/`)
            }
          } else {
            changes.push(Change.putDefinition(id, serialize(d.card), d.meta !== undefined ? serialize(d.meta) : null, d.link !== undefined ? serialize(d.link) : null))
          }
        }
        const written = new Set<string>()
        for (const [prefix, ours] of [...this.externalEdits].sort(([a], [b]) => a.localeCompare(b))) {
          if (ours.removed) continue
          const theirs = this.externals.get(prefix)
          const m = mergeExternal(this.externalBases.get(prefix) ?? undefined, ours, theirs)
          merged.set(prefix, m)
          const now = externalFiles(m), was = theirs ? externalFiles(theirs) : new Map<string, string>()
          for (const [rel, text] of [...now].sort(([a], [b]) => a.localeCompare(b))) {
            if (was.get(rel) === text) continue
            const p = prefix + rel
            written.add(p)
            changes.push(Change.putSource(p, new TextEncoder().encode(text), (theirs ? this.externalHashes.get(p) : this.sources.get(p)?.hash) ?? null))
          }
          for (const rel of [...was.keys()].sort()) {
            if (now.has(rel)) continue
            const p = prefix + rel
            const h = this.externalHashes.get(p) ?? this.sources.get(p)?.hash
            if (h && !this.sourceRemovals.has(p)) {
              written.add(p)
              changes.push(Change.removeSource(p, h))
            }
          }
        }
        const goesWithItsDefinition = (p: string) => removedFolders.some((f) => p.startsWith(f))
        for (const [p, text] of [...this.sourceEdits].sort(([a], [b]) => a.localeCompare(b))) {
          if (this.sourceRemovals.has(p) || written.has(p) || goesWithItsDefinition(p)) continue
          changes.push(Change.putSource(p, new TextEncoder().encode(text), this.sourceBases.get(p) ?? base.hashes.get(p) ?? null))
        }
        for (const p of [...this.sourceRemovals].sort()) {
          if (written.has(p) || goesWithItsDefinition(p)) continue
          const h = this.sourceBases.get(p) ?? base.hashes.get(p) ?? this.sources.get(p)?.hash
          if (h) changes.push(Change.removeSource(p, h))
        }
      }
      if (!changes.length) {
        if (!layoutOnly) this.forgetSaved(sent, false, new Map())
        return
      }
      const op = uuid()
      try {
        const renames = layoutOnly ? [] : [...this.pendingCardRenames]
        const result = await client.save(base.rev, op, this.origin, changes, renames)
        if (renames.length) this.pendingCardRenames.splice(0, renames.length)
        this.saveError = undefined
        this.conflictRetries = 0
        if (result.delta) this.applyDeltaFromSave(result.delta)
        this.forgetSaved(sent, layoutOnly, merged)
      } catch (e) {
        if (e instanceof RPCFailure) {
          switch (e.kind) {
            case D.FailureKind.revision_conflict:
            case D.FailureKind.hash_conflict: {
              const p = e.args.path
              if (e.kind === D.FailureKind.hash_conflict && p && !this.isExternalDocument(p)) {
                // A file this window edited was changed elsewhere too: kept, not merged.
                this.saveError = `${p} は別の場所でも変更されたため保存していません。変更は保持しています。`
                return
              }
              if (this.conflictRetries >= 8) {
                this.conflictRetries = 0
                this.saveError = '他の変更と重なり保存できませんでした。変更は保持しています。'
                return
              }
              // Newer changes are on their way (DocPatched): merge them in and try again.
              this.conflictRetries++
              const want = e.currentRev ?? this.rev + 1n
              for (let i = 0; i < 40 && this.rev < want; i++) await sleep(50)
              await sleep(30 * this.conflictRetries)
              this.saveAgain = true
              break
            }
            case D.FailureKind.busy:
              this.savingAfterRun = true
              break
            case D.FailureKind.external_change:
              this.saveError = `${e.args.path ?? 'ファイル'} が Tsugumi の外で変更されました。再読込で取り込めます（変更は保持しています）。`
              break
            default:
              this.saveError = errorText(e)
          }
        } else {
          // The reply may have been lost after the save committed: ask the core.
          const r = await client.operation(op).catch(() => undefined)
          if (r && r.state === D.OperationState.completed) {
            this.saveError = undefined
            this.forgetSaved(sent, layoutOnly, merged)
          } else {
            this.saveError = `保存できませんでした: ${errorText(e)}。変更は保持しています。`
          }
        }
      }
    } finally {
      this.saving = false
      this.changed()
    }
  }

  /** Whether a path is one of an external's documents (merged, not compared by hash). */
  private isExternalDocument(p: string): boolean {
    return [...this.effectiveExternals.keys()].some((pre) => p.startsWith(pre) && isDocument(p.slice(pre.length)))
  }

  /** After a save: the parts sent are confirmed now; edits made while it was
   *  on its way stay in the draft. */
  private forgetSaved(sent: Snapshot, layoutOnly: boolean, merged: Map<string, ExternalProject>) {
    this.layout = jeq(this.layout, sent.layout) ? clone(this.confirmedLayout) : clone(merge3(sent.layout, this.layout, this.confirmedLayout) ?? this.layout)
    if (!layoutOnly) {
      this.project = jeq(this.project, sent.project) ? clone(this.confirmedProject) : clone(merge3(sent.project, this.project, this.confirmedProject) ?? this.project)
      for (const [id, d] of sent.definitions) if (defEq(this.definitionEdits.get(id), d)) this.definitionEdits.delete(id)
      for (const [p, d] of sent.sources) {
        if (this.sourceEdits.get(p) === d) {
          this.sourceEdits.delete(p)
          this.sourceBases.delete(p)
        }
      }
      for (const p of sent.removals) {
        if (this.sourceRemovals.has(p) && !sent.sources.has(p)) {
          this.sourceRemovals.delete(p)
          this.sourceBases.delete(p)
        }
      }
      for (const [prefix, m] of merged) {
        // What was written is now the external's confirmed content.
        this.externals.set(prefix, m)
        for (const rel of externalFiles(m).keys()) {
          const h = this.sources.get(prefix + rel)?.hash
          if (h) this.externalHashes.set(prefix + rel, h)
        }
        if (extEq(this.externalEdits.get(prefix), sent.externals.get(prefix))) {
          this.externalEdits.delete(prefix)
          this.externalBases.delete(prefix)
        } else {
          this.externalBases.set(prefix, m)
        }
      }
      for (const [prefix, e] of sent.externals) {
        if (e.removed && extEq(this.externalEdits.get(prefix), e)) {
          this.externalEdits.delete(prefix)
          this.externalBases.delete(prefix)
          this.externals.delete(prefix)
        }
      }
      this.savingAfterRun = false
    }
    // Whatever is left was edited on top of what was just saved.
    this.draftBase = undefined
    this.settleDraftBase()
  }

  private applyDeltaFromSave(d: R.DocumentDeltaT) {
    if (d.baseRev === this.rev) {
      const keep = this.snapshot()
      this.applyDelta(d, true)
      this.project = keep.project
      this.layout = keep.layout
    }
  }

  async reloadExternal() {
    if (!this.client) return
    try {
      await this.client.reload(this.rev, uuid(), 'ui')
      this.saveError = undefined
    } catch (e) {
      this.saveError = errorText(e)
    }
    this.changed()
  }

  // ---- View state ------------------------------------------------------------------

  saveViewSoon(scope: string[], selection: Set<CardKey>) {
    clearTimeout(this.viewSaveTimer)
    this.viewSaveTimer = setTimeout(async () => {
      const client = this.client
      if (!client) return
      const v = new R.ViewStateT(
        cardPath(scope),
        [...this.cameras].map(([k, c]) => new R.CameraT(cardPath(path(k)), c.x, c.y, c.zoom)),
        [...selection].sort().map((k) => cardPath(path(k))),
      )
      try {
        const r = await client.saveView(this.viewId, this.viewRev, v)
        this.viewRev = r.viewRev ?? this.viewRev
      } catch (e) {
        if (e instanceof RPCFailure && e.kind === D.FailureKind.revision_conflict) this.viewRev = e.currentRev ?? this.viewRev
      }
    }, 600)
  }

  /** Cards whose displays the canvas shows: every Card that can run may send displays. */
  get displayedCards(): Set<CardKey> {
    return new Set([...this.resolution.keys()].filter((k) => ['code', 'logger'].includes(this.kind(k))))
  }

  async refreshVisibleDisplays() {
    if (!this.client || !this.subscription) return
    await this.client.setVisible(this.subscription, [...this.displayedCards].sort().map(path), true).catch(() => {})
  }

  // ---- Runs --------------------------------------------------------------------

  async run(card: string[], propagate: boolean) {
    const client = this.client
    if (!client) throw new RPCError('disconnected', '接続していません')
    if (this.computationDirty) await this.save()
    if (this.computationDirty) throw new RPCFailure({ kind: D.FailureKind.validation_failed, diagnostic: this.saveError ?? '変更を保存できないため実行できません。' })
    const parallel = Math.max(1, Math.min(64, prefs.maxConcurrency))
    try {
      const accepted = await client.run(this.rev, uuid(), card, propagate, parallel)
      this.runStatus = accepted.run ?? undefined
      this.cardStates = new Map()
      this.timeline = []
      this.changed()
    } catch (e) {
      if (e instanceof RPCError && e.code === 'disconnected') this.reconnect() // never re-send blindly
      throw e
    }
  }

  async cancelRun() {
    const id = this.currentRunId
    if (!this.client || !id) return
    try {
      this.runStatus = await this.client.cancel(id)
      this.changed()
    } catch {}
  }

  // ---- Document helpers ------------------------------------------------------------

  private extLink(prefix: string, id: string): string | undefined {
    if (prefix === '') return str(get(this.links.get(id), 'external'))
    const e = this.external(prefix)
    return e ? externalLinks(e).get(id) : undefined
  }

  /** The project a scope's Cards are written in: "" for this project,
   *  "externals/<name>/" (nested further) inside a Card in Card; undefined
   *  when the scope is not a Card in Card. */
  projectPrefix(scope: string[]): string | undefined {
    let prefix = ''
    for (const id of scope) {
      const name = this.extLink(prefix, id)
      if (!name) return undefined
      prefix += `externals/${name}/`
    }
    return prefix
  }

  /** An external's documents with the draft applied. */
  external(prefix: string): ExternalProject | undefined {
    const e = this.externalEdits.get(prefix)
    if (e) return e.removed ? undefined : e
    return this.externals.get(prefix)
  }

  private graph(scope: string[]): J | undefined {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) return undefined
    return prefix === '' ? get(this.project, 'canvas') : get(this.external(prefix)?.project, 'canvas')
  }

  /** Cards of a scope: this project's, or (inside a Card in Card) the external's. */
  cards(scope: string[]): J[] {
    return arr(get(this.graph(scope), 'cards')) ?? []
  }

  connections(scope: string[]): J[] {
    return arr(get(this.graph(scope), 'connections')) ?? []
  }

  /** Mutates a scope's canvas in the draft (inside a Card in Card: its external's). */
  updateGraph(scope: string[], fn: (g: JObj) => void) {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) return
    const ensure = (p: J): JObj | undefined => {
      if (!isObj(p)) return undefined
      let c = p.get('canvas')
      if (!isObj(c)) {
        c = obj([['cards', []], ['connections', []]])
        p.set('canvas', c)
      }
      return c as JObj
    }
    if (prefix === '') {
      const c = ensure(this.project)
      if (c) fn(c)
    } else {
      this.updateExternal(prefix, (e) => {
        const c = ensure(e.project)
        if (c) fn(c)
      })
    }
  }

  /** The layout a scope's Cards, notes and groups are kept in. */
  layoutOf(scope: string[]): J {
    const prefix = this.projectPrefix(scope)
    if (!prefix) return this.layout
    return this.external(prefix)?.layout ?? obj()
  }

  updateLayout(scope: string[], fn: (l: JObj) => void) {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) return
    if (prefix === '') {
      if (!isObj(this.layout)) this.layout = obj()
      fn(this.layout as JObj)
    } else {
      this.updateExternal(prefix, (e) => {
        if (!isObj(e.layout)) e.layout = obj()
        fn(e.layout as JObj)
      })
    }
  }

  /** A Card's key in the layout of its scope (inside a Card in Card: its id). */
  layoutKey(k: CardKey): string {
    const p = path(k)
    return p.length > 1 ? p[p.length - 1] : k
  }

  instance(k: CardKey): JObj | undefined {
    const p = path(k)
    const last = p[p.length - 1]
    if (last === undefined) return undefined
    const c = this.cards(p.slice(0, -1)).find((c) => str(get(c, 'id')) === last)
    return isObj(c) ? c : undefined
  }

  private split(k: CardKey): { id: string; prefix: string } | undefined {
    const p = path(k)
    const id = p[p.length - 1]
    const prefix = this.projectPrefix(p.slice(0, -1))
    return id === undefined || prefix === undefined ? undefined : { id, prefix }
  }

  /** The Card's definition files with the draft applied (its own project's, or the external's). */
  cardDefinitionDraft(k: CardKey): DefinitionDraft | undefined {
    const sp = this.split(k)
    if (!sp) return undefined
    return sp.prefix === '' ? this.definitionDrafts.get(sp.id) : this.external(sp.prefix)?.definitions.get(sp.id)
  }

  /** The Card's settings (its _card.json). */
  cardDefinition(k: CardKey): J | undefined {
    const d = this.cardDefinitionDraft(k)
    return d && d.link === undefined ? d.card : undefined
  }

  /** The Card's _meta.json. */
  cardMeta(k: CardKey): J | undefined {
    return this.cardDefinitionDraft(k)?.meta
  }

  /** The confirmed definition files of a Card (not the draft). */
  confirmedCardDefinitionDraft(k: CardKey): DefinitionDraft | undefined {
    const sp = this.split(k)
    if (!sp) return undefined
    return sp.prefix === '' ? this.confirmedDefinitionDrafts.get(sp.id) : this.externals.get(sp.prefix)?.definitions.get(sp.id)
  }

  /** Sets a Card's definition files in the draft (undefined removes them). */
  setCardDefinition(k: CardKey, d: DefinitionDraft | undefined) {
    const sp = this.split(k)
    if (!sp) return
    if (sp.prefix === '') this.setEffectiveDefinition(sp.id, d)
    else this.updateExternal(sp.prefix, (e) => {
      if (d) e.definitions.set(sp.id, d)
      else e.definitions.delete(sp.id)
    })
  }

  /** The external project of a Card in Card ("externals/<name>/…"), if it is one. */
  cardExternal(k: CardKey): string | undefined {
    const sp = this.split(k)
    if (!sp) return undefined
    const name = this.extLink(sp.prefix, sp.id)
    return name ? `${sp.prefix}externals/${name}/` : undefined
  }

  /** The folder of the Card's definition, project relative ("definitions/<id>/"). */
  definitionDir(k: CardKey): string | undefined {
    const sp = this.split(k)
    return sp ? `${sp.prefix}definitions/${sp.id}/` : undefined
  }

  /** A Card in Card's ports: its external's boundary. */
  boundary(k: CardKey): J | undefined {
    const ext = this.cardExternal(k)
    return ext ? get(this.external(ext)?.project, 'boundary') : undefined
  }

  /** Changes a Card's _card.json in the draft. */
  updateCard(k: CardKey, fn: (c: JObj) => void) {
    const d = this.cardDefinitionDraft(k)
    if (!d || d.link !== undefined) return
    const n = cloneDef(d)
    if (!isObj(n.card)) n.card = obj()
    fn(n.card as JObj)
    this.setCardDefinition(k, n)
  }

  /** Changes a Card's _meta.json in the draft. */
  updateMeta(k: CardKey, fn: (m: JObj) => void) {
    const d = this.cardDefinitionDraft(k)
    if (!d || d.link !== undefined) return
    const n = cloneDef(d)
    if (!isObj(n.meta)) n.meta = obj()
    fn(n.meta as JObj)
    this.setCardDefinition(k, n)
  }

  /** A setting of the Card (its _card.json). */
  cardSetting(k: CardKey, name: string): J | undefined {
    return get(this.cardDefinition(k), name)
  }

  position(k: CardKey): { x: number; y: number } {
    const p = get(this.layoutOf(path(k).slice(0, -1)), 'cards', this.layoutKey(k))
    return { x: dbl(get(p, 'x')) ?? 0, y: dbl(get(p, 'y')) ?? 0 }
  }

  setPosition(k: CardKey, pt: { x: number; y: number }) {
    const lk = this.layoutKey(k)
    this.updateLayout(path(k).slice(0, -1), (l) => {
      let cards = l.get('cards')
      if (!isObj(cards)) {
        cards = obj()
        l.set('cards', cards)
      }
      let entry = (cards as JObj).get(lk)
      if (!isObj(entry)) {
        entry = obj()
        ;(cards as JObj).set(lk, entry)
      }
      ;(entry as JObj).set('x', real(Math.round(pt.x * 10) / 10))
      ;(entry as JObj).set('y', real(Math.round(pt.y * 10) / 10))
    })
  }

  /** Reads the externals' documents when their sources changed. */
  refreshExternals() {
    const paths = [...this.sources.keys()].filter((p) => p.startsWith('externals/')).sort()
    const k = paths.map((p) => `${p}=${this.sources.get(p)?.hash ?? ''}`).join(';')
    if (k === this.externalsKey) return
    this.externalsKey = k
    const client = this.client
    if (!client) {
      // Not connected yet (first snapshot): read once the connection is there.
      this.externalsKey = ''
      setTimeout(() => this.client && this.refreshExternals(), 0)
      return
    }
    const atRev = this.rev
    const hashes = new Map([...this.sources].map(([p, v]) => [p, v.hash]))
    void (async () => {
      const found = new Map<string, ExternalProject>()
      const read = new Map<string, string>()
      const doc = async (rel: string): Promise<J | undefined> => {
        const h = hashes.get(rel)
        if (!h) return undefined
        try {
          const d = await client.readFile(rel, atRev)
          read.set(rel, h)
          return tryParse(new TextDecoder().decode(d))
        } catch {
          return undefined
        }
      }
      for (const p of paths) {
        if (!p.endsWith('/tsugumi.json')) continue
        const prefix = p.slice(0, -'tsugumi.json'.length)
        // Only folders that are externals of a project: …externals/<name>/tsugumi.json.
        const parts = prefix.split('/').filter(Boolean)
        if (parts.length < 2 || parts.length % 2 !== 0 || parts[parts.length - 2] !== 'externals') continue
        const proj = await doc(p)
        if (proj === undefined) continue
        const ext: ExternalProject = { project: proj, layout: (await doc(prefix + 'layout.json')) ?? obj(), definitions: new Map() }
        // Every definition folder of the external (placed or not).
        const ids = new Set<string>()
        for (const q of paths) {
          if (!q.startsWith(prefix + 'definitions/')) continue
          const rest = q.slice((prefix + 'definitions/').length).split('/')
          if (rest.length === 2 && ['_card.json', '_link.json'].includes(rest[1])) ids.add(rest[0])
        }
        for (const id of [...ids].sort()) {
          const dir = `${prefix}definitions/${id}/`
          const meta = await doc(dir + '_meta.json')
          const l = await doc(dir + '_link.json')
          if (l !== undefined) ext.definitions.set(id, { card: obj(), meta, link: l })
          else {
            const c = await doc(dir + '_card.json')
            if (c !== undefined) ext.definitions.set(id, { card: c, meta })
          }
        }
        found.set(prefix, ext)
      }
      if (this.externalsKey !== k) return
      this.externals = found
      this.externalHashes = read
      this.normalizeExternalEdits()
      this.settleDraftBase()
      this.changed()
      // Cards inside Cards in Card are known now: their displays are wanted too.
      await this.refreshVisibleDisplays()
    })()
  }

  // ---- Reading -------------------------------------------------------------------

  kind(k: CardKey): CardKind {
    if (this.cardExternal(k) !== undefined) return 'subgraph'
    const def = this.cardDefinition(k)
    if (def === undefined) return 'draft'
    return definitionKind(def)
  }

  /** A Card's name is its id (and its definition folder). */
  displayName(k: CardKey): string {
    return path(k).pop() ?? k
  }

  scopeName(scope: string[]): string {
    return scope.length === 0 ? str(get(this.project, 'name')) || this.folderName : this.displayName(key(scope))
  }

  /** The Cards in Card of a scope, for the hierarchy. */
  childScopes(scope: string[]): string[][] {
    return this.cards(scope).flatMap((c) => {
      const id = str(get(c, 'id'))
      return id && this.cardExternal(key([...scope, id])) !== undefined ? [[...scope, id]] : []
    })
  }

  /** Whether a Card is one of this project's own (not inside a Card in Card). */
  isOwnCard(k: CardKey): boolean {
    return path(k).length === 1
  }

  /** Definition folders of this project that no Card uses. */
  get unplacedDefinitions(): string[] {
    const used = new Set(this.cards([]).map((c) => str(get(c, 'id'))))
    return [...new Set([...this.definitions.keys(), ...this.links.keys()])].filter((id) => !used.has(id)).sort()
  }

  /** Places a definition folder no Card uses as its Card (the same name). */
  placeDefinition(id: string, at: { x: number; y: number }): CardKey {
    const k = key([id])
    if (!this.unplacedDefinitions.includes(id)) return k
    this.edit((x) => {
      x.appendCard(id, [])
      x.setPosition(k, at)
    })
    return k
  }

  /** Source text as the draft sees it. */
  async sourceText(p: string): Promise<string> {
    const e = this.sourceEdits.get(p)
    if (e !== undefined) return e
    if (!this.client) throw new RPCError('disconnected', '接続していません')
    return new TextDecoder().decode(await this.client.readFile(p, this.rev))
  }

  collapsed(k: CardKey): boolean {
    return bool(get(this.layoutOf(path(k).slice(0, -1)), 'cards', this.layoutKey(k), 'collapsed')) ?? false
  }

  // ---- Cards and connections ---------------------------------------------------------

  /** A free Card name in a scope's project: not a Card, a definition folder or an external there. */
  uniqueId(base: string, scope: string[] = []): string {
    const prefix = this.projectPrefix(scope) ?? ''
    const existing = new Set(this.cards(scope).map((c) => str(get(c, 'id'))).filter((x): x is string => !!x))
    if (prefix === '') {
      for (const id of [...this.definitions.keys(), ...this.links.keys(), ...this.confirmedDefinitions.keys(), ...this.confirmedLinks.keys()]) existing.add(id)
    } else {
      for (const id of this.external(prefix)?.definitions.keys() ?? []) existing.add(id)
    }
    const ext = prefix + 'externals/'
    for (const p of [...this.sourcePaths, ...this.effectiveExternals.keys()]) {
      if (p.startsWith(ext)) existing.add(p.slice(ext.length).split('/')[0])
    }
    let stem = base.toLowerCase().replace(/[^\p{L}\p{N}_-]/gu, '-').replace(/^-+|-+$/g, '')
    if (!stem) stem = 'card'
    let id = stem
    for (let n = 2; existing.has(id); n++) id = `${stem}-${n}`
    return id
  }

  /** Adds a Card instance to a scope's canvas. */
  appendCard(id: string, scope: string[]) {
    this.startClean(key([...scope, id]))
    this.updateGraph(scope, (g) => g.set('cards', [...(arr(g.get('cards')) ?? []), obj([['id', id]])]))
  }

  /** A new Card's name can be one a Card had: nothing of that Card stays with it.
   *  The core keeps this project's layout up with renames and deletes, but a Card
   *  in Card's layout or a file written by hand can still hold a frame. */
  private startClean(k: CardKey) {
    this.displays.delete(k)
    this.cardStates.delete(k)
    this.updateLayout(path(k).slice(0, -1), (l) => this.dropLayoutEntries(k, l))
  }

  /** The Card's place and frame (and the places of anything inside it). */
  private dropLayoutEntries(k: CardKey, l: JObj) {
    const lk = this.layoutKey(k)
    const cards = l.get('cards')
    if (isObj(cards)) for (const kk of cards.keys()) if (kk === lk || kk.startsWith(lk + '/')) cards.set(kk, undefined)
    const displays = l.get('displays')
    if (isObj(displays)) displays.set(lk, undefined)
  }

  /** A new Card from a template, with its own definition folder. `place` says
   *  where instead of `at`, once the Card's size is known. */
  addCard(t: DefinitionTemplate, scope: string[], at: { x: number; y: number }, name?: string, place?: (k: CardKey) => { x: number; y: number }): CardKey {
    const id = this.uniqueId(name ?? templateBaseName(t), scope)
    const k = key([...scope, id])
    this.edit((x) => {
      x.appendCard(id, scope)
      x.writeTemplate(t, k)
      x.setPosition(k, place ? place(k) : at)
    })
    return k
  }

  /** Copies the definition `id` of the project at `prefix` ("" for this one)
   *  into a scope as a new Card: settings and every file. A Card in Card
   *  brings its own copy of the external. */
  async copyDefinition(prefix: string, id: string, scope: string[], name?: string): Promise<{ id: string; apply: (s: ProjectSession) => void } | undefined> {
    const d = prefix === '' ? this.definitionDrafts.get(id) : this.external(prefix)?.definitions.get(id)
    if (!d) return undefined
    const dir = `${prefix}definitions/${id}/`
    const newId = this.uniqueId(name ?? id, scope)
    const toPrefix = this.projectPrefix(scope) ?? ''
    const files = new Map<string, string>()
    for (const p of this.sourcePaths) {
      if (!p.startsWith(dir)) continue
      const rel = p.slice(dir.length)
      if (['_card.json', '_link.json', '_meta.json'].includes(rel)) continue
      try {
        files.set(`${toPrefix}definitions/${newId}/${rel}`, await this.sourceText(p))
      } catch {}
    }
    let link = d.link
    let copy: ExternalCopy | undefined
    const extName = str(get(link, 'external'))
    if (extName) {
      const n = this.uniqueId(extName, scope)
      copy = await this.copyExternal(`${prefix}externals/${extName}/`, `${toPrefix}externals/${n}/`)
      link = obj([['external', n]])
    }
    const draft: DefinitionDraft = { card: clone(d.card), meta: clone(d.meta), link: clone(link) }
    const newKey = key([...scope, newId])
    return {
      id: newId,
      apply: (sess) => {
        sess.setCardDefinition(newKey, draft)
        for (const [p, t] of files) {
          sess.sourceRemovals.delete(p)
          sess.sourceEdits.set(p, t)
        }
        copy?.apply(sess)
      },
    }
  }

  /** Copies the external at `from` (with the externals in it) to the folder `to`. */
  async copyExternal(from: string, to: string): Promise<ExternalCopy> {
    const c = new ExternalCopy()
    const inside = [...this.effectiveExternals].filter(([p]) => p.startsWith(from))
    for (const [p, e] of inside) c.docs.set(to + p.slice(from.length), cloneExternal(e))
    for (const p of this.sourcePaths) {
      if (!p.startsWith(from)) continue
      // Documents come from the parsed externals; every other file is copied.
      if (inside.some(([pre]) => p.startsWith(pre) && isDocument(p.slice(pre.length)))) continue
      try {
        c.files.set(to + p.slice(from.length), await this.sourceText(p))
      } catch {}
    }
    return c
  }

  /** Places a copy of an external (`from`) in a scope as a new Card in Card. */
  async placeExternalCopy(from: string, scope: string[], at: { x: number; y: number }): Promise<CardKey | undefined> {
    if (!this.external(from)) return undefined
    const base = from.split('/').filter(Boolean).pop() ?? 'sub'
    const name = this.uniqueId(base, scope)
    const toPrefix = this.projectPrefix(scope) ?? ''
    const copy = await this.copyExternal(from, `${toPrefix}externals/${name}/`)
    const k = key([...scope, name])
    this.edit((x) => {
      copy.apply(x)
      x.setCardDefinition(k, { card: obj(), link: obj([['external', name]]) })
      x.appendCard(name, scope)
      x.setPosition(k, at)
    })
    return k
  }

  /** Places a copy of a definition of an external (or of this project) as a new Card. */
  async placeDefinitionCopy(prefix: string, id: string, scope: string[], at: { x: number; y: number }): Promise<CardKey | undefined> {
    const c = await this.copyDefinition(prefix, id, scope)
    if (!c) return undefined
    const k = key([...scope, c.id])
    this.edit((x) => {
      c.apply(x)
      x.appendCard(c.id, scope)
      x.setPosition(k, at)
    })
    return k
  }

  /** Takes in another Tsugumi project as a Card in Card: its folder (read by
   *  the host server, without hidden folders) goes to externals/<name>/. */
  async importProject(folder: string, scope: string[], at: { x: number; y: number }): Promise<CardKey> {
    const { name: folderName, files } = await api<{ name: string; files: Record<string, string> }>('/api/import/project', { folder })
    const json = (rel: string) => (files[rel] !== undefined ? tryParse(files[rel]) : undefined)
    // The project and the externals in it, each with its documents.
    const projects = Object.keys(files).filter((rel) => {
      if (!rel.endsWith('tsugumi.json')) return false
      const parts = rel.split('/').slice(0, -1)
      return parts.length % 2 === 0 && parts.every((p, i) => i % 2 === 1 || p === 'externals')
    }).map((rel) => rel.slice(0, -'tsugumi.json'.length))
    const name = this.uniqueId(folderName, scope)
    const to = `${this.projectPrefix(scope) ?? ''}externals/${name}/`
    const docs = new Map<string, ExternalProject>()
    for (const p of projects) {
      const proj = json(p + 'tsugumi.json')
      if (proj === undefined) continue
      const ext: ExternalProject = { project: proj, layout: json(p + 'layout.json') ?? obj(), definitions: new Map() }
      for (const rel of Object.keys(files)) {
        if (!rel.startsWith(p + 'definitions/')) continue
        const rest = rel.slice((p + 'definitions/').length).split('/')
        if (rest.length !== 2) continue
        const id = rest[0], dir = `${p}definitions/${id}/`
        if (rest[1] === '_link.json') {
          const l = json(rel)
          if (l !== undefined) ext.definitions.set(id, { card: obj(), meta: json(dir + '_meta.json'), link: l })
        }
        if (rest[1] === '_card.json') {
          const c = json(rel)
          if (c !== undefined) ext.definitions.set(id, { card: c, meta: json(dir + '_meta.json') })
        }
      }
      docs.set(to + p, ext)
    }
    const k = key([...scope, name])
    this.edit((x) => {
      for (const [rel, text] of Object.entries(files)) {
        if (projects.some((p) => rel.startsWith(p) && isDocument(rel.slice(p.length)))) continue
        x.sourceRemovals.delete(to + rel)
        x.sourceEdits.set(to + rel, text)
      }
      for (const [p, e] of [...docs].sort(([a], [b]) => a.localeCompare(b))) x.createExternal(p, e)
      x.setCardDefinition(k, { card: obj(), link: obj([['external', name]]) })
      x.appendCard(name, scope)
      x.setPosition(k, at)
    })
    return k
  }

  /** Copies Cards with their definition folders (settings, sources) under new
   *  names; connections between the copied Cards are copied too. */
  async duplicate(keys: Set<CardKey>, scope: string[]): Promise<Set<CardKey>> {
    const map = new Map<string, string>()
    const applies: ((sess: ProjectSession) => void)[] = []
    const taken = new Set<string>()
    const prefix = this.projectPrefix(scope) ?? ''
    for (const k of [...keys].sort()) {
      const id = path(k).pop()
      if (!id) continue
      let want = id
      for (let n = 2; taken.has(this.uniqueId(want, scope)); n++) want = `${id}-${n}`
      const c = await this.copyDefinition(prefix, id, scope, this.uniqueId(want, scope))
      if (!c) continue
      taken.add(c.id)
      map.set(id, c.id)
      applies.push(c.apply)
    }
    const made = new Set<CardKey>()
    this.edit((x) => {
      for (const a of applies) a(x)
      for (const nw of map.values()) x.startClean(key([...scope, nw]))
      x.updateGraph(scope, (g) => {
        const cards = [...(arr(g.get('cards')) ?? [])]
        const conns = [...(arr(g.get('connections')) ?? [])]
        for (const [, nw] of [...map].sort(([a], [b]) => a.localeCompare(b))) cards.push(obj([['id', nw]]))
        for (const c of [...conns]) {
          const f = str(get(c, 'from', 'card')), t = str(get(c, 'to', 'card'))
          if (f && t && map.has(f) && map.has(t)) {
            const copy = clone(c) as JObj
            ;(copy.get('from') as JObj).set('card', map.get(f)!)
            ;(copy.get('to') as JObj).set('card', map.get(t)!)
            conns.push(copy)
          }
        }
        g.set('cards', cards)
        g.set('connections', conns)
      })
      for (const [old, nw] of map) {
        const p = x.position(key([...scope, old]))
        x.setPosition(key([...scope, nw]), { x: p.x + 30, y: p.y + 30 })
        made.add(key([...scope, nw]))
      }
    })
    return made
  }

  /** Renames a Card (its id, its definition folder): saved at once, with its
   *  connections and positions following (and its run history). */
  /** Why a Card cannot take a name (undefined: it can). */
  cardNameProblem(k: CardKey, raw: string): string | undefined {
    const nw = raw.trim()
    if (nw === path(k).pop()) return undefined
    if (!nw || nw.length > 128 || nw === '.' || nw === '..' || !/^[A-Za-z0-9._-]+$/.test(nw)) return '名前に使えるのは英数字と - _ . だけです（128 文字まで）。'
    if (this.uniqueId(nw, path(k).slice(0, -1)) !== nw) return `「${nw}」はすでに使われています。`
    return undefined
  }

  // ---- A Draft written on the canvas -------------------------------------------------

  /** Whether a Draft has no name of its own: its id is one made for it. */
  draftUntitled(k: CardKey): boolean {
    return this.kind(k) === 'draft' && /^draft(-[0-9]+)?$/.test(this.displayName(k))
  }

  /** Starts writing a Draft's name and content on its Card. */
  startDraftEdit(k: CardKey) {
    this.inlineDraft = { card: k, name: this.draftUntitled(k) ? '' : this.displayName(k), content: str(get(this.cardMeta(k), 'content')) ?? '' }
    this.changed()
  }

  /** Why the name typed on a Draft's Card cannot be taken (no name is fine). */
  draftNameProblem(k: CardKey, name: string): string | undefined {
    return name.trim() ? this.cardNameProblem(k, name) : undefined
  }

  setDraftEdit(change: Partial<Pick<InlineDraft, 'name' | 'content'>>) {
    if (!this.inlineDraft) return
    this.inlineDraft = { ...this.inlineDraft, ...change }
    this.changed()
  }

  /** Keeps what was written. A name the Card cannot take stays as it was, with
   *  a notice. The Card's key when it was renamed. */
  async finishDraftEdit(): Promise<CardKey | undefined> {
    const e = this.inlineDraft
    if (!e) return undefined
    this.inlineDraft = undefined
    this.changed()
    if (!this.instance(e.card)) return undefined
    if (e.content !== (str(get(this.cardMeta(e.card), 'content')) ?? '')) {
      this.edit((x) => x.updateMeta(e.card, (m) => m.set('content', e.content || undefined)))
    }
    // No name: an id is made for it (a Draft that has none keeps the one it has).
    if (!e.name.trim() && this.draftUntitled(e.card)) return undefined
    const name = e.name.trim() || this.uniqueId('draft', path(e.card).slice(0, -1))
    if (name === this.displayName(e.card)) return undefined
    try {
      await this.renameCard(e.card, name)
      return key([...path(e.card).slice(0, -1), name])
    } catch (err) {
      this.notice = err instanceof RPCFailure ? err.diagnostic ?? errorText(err) : errorText(err)
      this.changed()
      return undefined
    }
  }

  async renameCard(k: CardKey, raw: string): Promise<void> {
    const old = path(k).pop()
    if (!old) return
    const scope = path(k).slice(0, -1)
    const nw = raw.trim()
    if (nw === old) return
    const problem = this.cardNameProblem(k, nw)
    if (problem) throw new RPCFailure({ kind: D.FailureKind.invalid_request, diagnostic: problem })
    if (this.dirty) await this.save()
    if (this.dirty) throw new RPCFailure({ kind: D.FailureKind.validation_failed, diagnostic: this.saveError ?? '変更を保存できませんでした。' })
    const rpc = this.client
    if (!rpc) throw new RPCError('disconnected', '接続していません')
    const renamedGraph = (g: J) => {
      const out = clone(isObj(g) ? g : obj()) as JObj
      out.set('cards', (arr(out.get('cards')) ?? []).map((c) => (str(get(c, 'id')) === old ? obj([['id', nw]]) : c)))
      out.set('connections', (arr(out.get('connections')) ?? []).map((c) => {
        const cc = clone(c) as JObj
        for (const side of ['from', 'to']) {
          const e = cc.get(side)
          if (isObj(e) && str(e.get('card')) === old) e.set('card', nw)
        }
        return cc
      }))
      return out
    }
    const renamedLayout = (l: J) => {
      const lay = clone(isObj(l) ? l : obj()) as JObj
      const rk = (x: string) => (x === old ? nw : x.startsWith(old + '/') ? nw + x.slice(old.length) : x)
      for (const field of ['cards', 'displays']) {
        const o = lay.get(field)
        if (isObj(o)) lay.set(field, new JObj(o.entries().map(([kk, v]) => [rk(kk), v])))
      }
      for (const field of ['notes', 'groups']) {
        const a = arr(lay.get(field))
        if (!a) continue
        lay.set(field, a.map((item) => {
          const it = clone(item)
          if (!isObj(it)) return it
          const cs = arr(it.get('cards'))
          if (cs) it.set('cards', cs.map((c) => (str(c) === old ? nw : c)))
          const outs = arr(it.get('outputs'))
          if (outs) {
            it.set('outputs', outs.map((o) => {
              const oo = clone(o)
              if (isObj(oo) && str(oo.get('card')) === old) oo.set('card', nw)
              return oo
            }))
          }
          return it
        }))
      }
      return lay
    }
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) return
    if (prefix === '') {
      const proj = clone(this.confirmedProject) as JObj
      proj.set('canvas', renamedGraph(proj.get('canvas') ?? obj()))
      const lay = renamedLayout(this.confirmedLayout)
      const changes = [Change.setProject(serialize(proj)), Change.renameDefinition(old, nw)]
      if (!jeq(lay, this.confirmedLayout)) changes.push(Change.setLayout(serialize(lay)))
      await rpc.save(this.rev, uuid(), this.origin, changes)
      return
    }
    // Inside a Card in Card: the external's documents and files are rewritten.
    const from = `${prefix}definitions/${old}/`, to = `${prefix}definitions/${nw}/`
    const files = new Map<string, string>()
    for (const p of this.sourcePaths) {
      if (p.startsWith(from) && !isDocument(p.slice(prefix.length))) files.set(to + p.slice(from.length), await this.sourceText(p))
    }
    this.edit((x) => {
      x.updateExternal(prefix, (e) => {
        if (isObj(e.project)) {
          e.project.set('canvas', renamedGraph(e.project.get('canvas') ?? obj()))
          const b = e.project.get('boundary')
          if (isObj(b)) {
            for (const side of ['inputs', 'outputs']) {
              const o = b.get(side)
              if (!isObj(o)) continue
              for (const [, v] of o.entries()) if (isObj(v) && str(v.get('card')) === old) v.set('card', nw)
            }
          }
        }
        e.layout = renamedLayout(e.layout)
        const d = e.definitions.get(old)
        if (d) e.definitions.set(nw, d)
        e.definitions.delete(old)
      })
      for (const p of x.sourcePaths) if (p.startsWith(from) && !isDocument(p.slice(prefix.length))) x.removeSource(p)
      for (const [p, t] of files) {
        x.sourceRemovals.delete(p)
        x.sourceEdits.set(p, t)
      }
    })
    // The run history of the Card (and of anything inside it) follows the name.
    this.pendingCardRenames.push({ from: [...scope, old], to: [...scope, nw] })
    await this.save()
  }

  deleteCards(keys: Set<CardKey>, scope: string[]) {
    this.edit((x) => x.removeCards(keys, scope))
  }

  /** A step of an edit: see deleteCards. */
  removeCards(keys: Set<CardKey>, scope: string[]) {
    const ids = new Set([...keys].map((k) => path(k).pop()!).filter(Boolean))
    if (!ids.size) return
    const prefix = this.projectPrefix(scope) ?? ''
    const x = this
    // Each Card's definition folder goes with it, and a Card in Card's own external copy.
    for (const k of keys) {
      const ext = x.cardExternal(k)
      if (ext) x.removeExternal(ext)
      x.removeCardDefinition(k)
    }
    const cut = x.connections(scope).filter((c) => ids.has(str(get(c, 'from', 'card')) ?? '') || ids.has(str(get(c, 'to', 'card')) ?? ''))
    x.updateGraph(scope, (g) => {
      g.set('cards', (arr(g.get('cards')) ?? []).filter((c) => !ids.has(str(get(c, 'id')) ?? '')))
      g.set('connections', (arr(g.get('connections')) ?? []).filter((c) => !cut.includes(c)))
    })
    x.dropDraftPorts(cut, scope)
    if (prefix) {
      // Boundary ports of removed Cards go too.
      x.updateExternal(prefix, (e) => {
        const b = get(e.project, 'boundary')
        if (!isObj(b)) return
        for (const side of ['inputs', 'outputs']) {
          const o = b.get(side)
          if (isObj(o)) b.set(side, new JObj(o.entries().filter(([, v]) => !ids.has(str(get(v, 'card')) ?? ''))))
        }
      })
    }
    x.updateLayout(scope, (l) => {
      for (const k of keys) x.dropLayoutEntries(k, l)
    })
    x.updateGroups(scope, (groups) => {
      for (const g of groups) if (isObj(g)) g.set('cards', (arr(g.get('cards')) ?? []).filter((c) => !ids.has(str(c) ?? '')))
    })
  }

  connect(from: [CardKey, string], to: [CardKey, string], scope: string[]) {
    const fromId = path(from[0]).pop(), toId = path(to[0]).pop()
    if (!fromId || !toId) return
    this.edit((x) => {
      // An end at NEW_PORT makes the Draft's port now.
      const fromPort = x.draftPort(from, true, to[1], scope)
      const toPort = x.draftPort(to, false, fromPort, scope)
      // One wire per output: a value goes to several Cards through a fork Card.
      // A second wire from an output inserts one (or, from a fork, adds a branch).
      const taken = x.connections(scope).filter((c) =>
        str(get(c, 'from', 'card')) === fromId && str(get(c, 'from', 'port')) === fromPort
        && !(str(get(c, 'to', 'card')) === toId && str(get(c, 'to', 'port')) === toPort))
      let source: [string, string] = [fromId, fromPort]
      if (taken.length) {
        const nextId = taken.length === 1 ? str(get(taken[0], 'to', 'card')) : undefined
        const next = nextId ? key([...scope, nextId]) : undefined
        if (x.kind(from[0]) === 'fork') source = [fromId, x.addForkBranch(from[0], toId)]
        else if (next && nextId && x.kind(next) === 'fork') source = [nextId, x.addForkBranch(next, toId)]  // one more branch of it
        else {
          const fork = x.insertFork([fromId, fromPort], taken, scope)
          source = [fork, x.addForkBranch(key([...scope, fork]), toId)]
        }
      }
      // One connection per input port: a new wire replaces the old one.
      const replaced = x.connections(scope).filter((c) => str(get(c, 'to', 'card')) === toId && str(get(c, 'to', 'port')) === toPort)
      x.updateGraph(scope, (g) => {
        const conns = (arr(g.get('connections')) ?? []).filter((c) => !replaced.includes(c))
        conns.push(obj([['from', obj([['card', source[0]], ['port', source[1]]])], ['to', obj([['card', toId], ['port', toPort]])]]))
        g.set('connections', conns)
      })
      x.dropDraftPorts(replaced, scope)
    })
  }

  /** Whether a wire of the scope ends at this port. */
  portWired(k: CardKey, port: string, output: boolean, scope: string[]): boolean {
    const id = path(k).pop()
    const side = output ? 'from' : 'to'
    return this.connections(scope).some((c) => str(get(c, side, 'card')) === id && str(get(c, side, 'port')) === port)
  }

  /** The port a wire's end names. At NEW_PORT: a Draft's port without a wire
   *  or a new one, named after the port at the wire's other end when that says
   *  more than "in" or "out". A Draft's ports are declared by its wires. */
  private draftPort(end: [CardKey, string], output: boolean, other: string, scope: string[]): string {
    if (end[1] !== NEW_PORT) return end[1]
    const ports = this.portData(end[0])
    const names = (output ? ports.outputs : ports.inputs).map((p) => p.name)
    const free = names.find((n) => !this.portWired(end[0], n, output, scope))
    if (free) return free
    const plain = output ? 'out' : 'in'
    const base = /^[A-Za-z_][A-Za-z0-9_]*$/.test(other) && !/^(in|out)[0-9]*$/.test(other) ? other : plain
    let name = base
    for (let n = 2; names.includes(name); n++) name = `${base}${n}`
    const note = obj([['name', name]])
    if (output) this.updateMeta(end[0], (m) => m.set('outputs', [...(arr(m.get('outputs')) ?? []), note]))
    else this.updateCard(end[0], (c) => c.set('inputs', [...(arr(c.get('inputs')) ?? []), note]))
    return name
  }

  /** After wires are gone: a Draft's port goes with its last wire. */
  private dropDraftPorts(removed: J[], scope: string[]) {
    const held = (k: CardKey, port: string, output: boolean) => {
      if (this.portWired(k, port, output, scope)) return true
      // Inside a Card in Card, the outer Card's boundary may name it.
      const side = get(scope.length ? this.boundary(key(scope)) : undefined, output ? 'outputs' : 'inputs')
      return isObj(side) && side.entries().some(([, e]) => str(get(e, 'card')) === path(k).pop() && str(get(e, 'port')) === port)
    }
    const without = (list: J | undefined, port: string) => {
      const rest = (arr(list) ?? []).filter((p) => str(get(p, 'name')) !== port)
      return rest.length ? rest : undefined
    }
    for (const c of removed) {
      for (const [side, output] of [['from', true], ['to', false]] as const) {
        const id = str(get(c, side, 'card')), port = str(get(c, side, 'port'))
        if (!id || !port) continue
        const k = key([...scope, id])
        if (!this.instance(k) || this.kind(k) !== 'draft' || held(k, port, output)) continue
        if (!output) this.updateCard(k, (d) => d.set('inputs', without(d.get('inputs'), port)))
        this.updateMeta(k, (m) => m.set(output ? 'outputs' : 'inputs', without(m.get(output ? 'outputs' : 'inputs'), port)))
      }
    }
  }

  /** A fork Card after an output: the output's wires (`wires`) now leave the
   *  fork, a branch each, and the output feeds the fork. Its id. */
  insertFork(from: [string, string], wires: J[], scope: string[]): string {
    const id = this.uniqueId('fork', scope)
    const k = key([...scope, id])
    this.appendCard(id, scope)
    this.writeTemplate('fork', k)
    this.updateCard(k, (c) => c.set('params', [obj([['name', 'branches'], ['type', 'string[]'], ['data', []]])]))
    // Between the output's Card and the first Card it fed.
    const a = this.position(key([...scope, from[0]]))
    const firstId = wires.length ? str(get(wires[0], 'to', 'card')) : undefined
    const b = firstId ? this.position(key([...scope, firstId])) : { x: a.x + 300, y: a.y }
    this.setPosition(k, { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 })
    const branches = wires.map((w) => this.addForkBranch(k, str(get(w, 'to', 'card')) ?? 'out'))
    this.updateGraph(scope, (g) => {
      const conns = (arr(g.get('connections')) ?? []).filter((c) => !wires.includes(c))
      wires.forEach((w, i) => conns.push(obj([['from', obj([['card', id], ['port', branches[i]]])], ['to', get(w, 'to') ?? null]])))
      conns.push(obj([['from', obj([['card', from[0]], ['port', from[1]]])], ['to', obj([['card', id], ['port', 'in']])]]))
      g.set('connections', conns)
    })
    return id
  }

  /** A new output of a fork Card, named after the Card it will feed. Its name. */
  addForkBranch(k: CardKey, target: string): string {
    const params = arr(get(this.cardDefinition(k), 'params')) ?? []
    const data = arr(get(params.find((p) => str(get(p, 'name')) === 'branches'), 'data'))
    const branches = data ? data.map((x) => str(x) ?? '') : ['out1', 'out2']
    const base = target || 'out'
    let name = base
    for (let n = 2; branches.includes(name); n++) name = `${base}_${n}`
    branches.push(name)
    this.updateCard(k, (c) => {
      const rest = (arr(c.get('params')) ?? []).filter((p) => str(get(p, 'name')) !== 'branches')
      c.set('params', [...rest, obj([['name', 'branches'], ['type', 'string[]'], ['data', branches]])])
    })
    return name
  }

  removeConnection(index: number, scope: string[]) {
    this.edit((x) => {
      const conns = [...x.connections(scope)]
      if (index >= conns.length) return
      const [removed] = conns.splice(index, 1)
      x.updateGraph(scope, (g) => g.set('connections', conns))
      x.dropDraftPorts([removed], scope)
    })
  }

  /** Removes connections (as they are in the scope's canvas). */
  removeConnections(list: J[], scope: string[]) {
    if (!list.length) return
    this.edit((x) => {
      x.updateGraph(scope, (g) => g.set('connections', (arr(g.get('connections')) ?? []).filter((c) => !list.some((l) => jeq(l, c)))))
      x.dropDraftPorts(list, scope)
    })
  }

  disconnect(k: CardKey, inputs: boolean, scope: string[]) {
    const id = path(k).pop()
    if (!id) return
    this.edit((x) => {
      const removed = x.connections(scope).filter((c) => (str(get(c, inputs ? 'to' : 'from', 'card')) ?? '') === id)
      x.updateGraph(scope, (g) => g.set('connections', (arr(g.get('connections')) ?? []).filter((c) => !removed.includes(c))))
      x.dropDraftPorts(removed, scope)
    })
  }

  setCollapsed(keys: Iterable<CardKey>, value: boolean) {
    this.edit((x) => {
      for (const k of keys) {
        const lk = x.layoutKey(k)
        x.updateLayout(path(k).slice(0, -1), (l) => {
          let cards = l.get('cards')
          if (!isObj(cards)) {
            cards = obj()
            l.set('cards', cards)
          }
          let entry = (cards as JObj).get(lk)
          if (!isObj(entry)) {
            entry = obj([['x', real(0)], ['y', real(0)]])
            ;(cards as JObj).set(lk, entry)
          }
          ;(entry as JObj).set('collapsed', value ? true : undefined)
        })
      }
    })
  }

  /** Sets a param's value (the `data` of the param in the Card's _card.json). */
  setParam(k: CardKey, name: string, value: J | undefined) {
    this.edit((x) => {
      x.updateCard(k, (c) => {
        const params = (arr(c.get('params')) ?? []).map((p) => clone(p))
        const p = params.find((p) => str(get(p, 'name')) === name)
        if (isObj(p)) p.set('data', value)
        c.set('params', params)
      })
    })
  }

  /** Moves the selected Cards into a new Card in Card: a new external project
   *  with their definitions and the wires between them. Wires crossing the
   *  selection become its boundary ports, so the graph computes the same. */
  async makeSubgraph(keys: Set<CardKey>, scope: string[], name = 'sub'): Promise<CardKey | undefined> {
    const ids = new Set([...keys].map((k) => path(k).pop()!).filter(Boolean))
    const scopePrefix = this.projectPrefix(scope)
    if (!ids.size || scopePrefix === undefined) return undefined
    const newId = this.uniqueId(name, scope)
    const newKey = key([...scope, newId])
    const ext = `${scopePrefix}externals/${newId}/`
    // Files of the Cards' definition folders, and of their own externals.
    const files = new Map<string, string>()
    const moved = new Map<string, ExternalProject>()
    for (const k of keys) {
      const dir = this.definitionDir(k), id = path(k).pop()
      if (!dir || !id) continue
      for (const p of this.sourcePaths) {
        if (p.startsWith(dir) && !isDocument(p.slice(scopePrefix.length))) files.set(`${ext}definitions/${id}/${p.slice(dir.length)}`, await this.sourceText(p).catch(() => ''))
      }
      const inner = this.cardExternal(k)
      const e = inner ? this.external(inner) : undefined
      const n = str(get(this.cardDefinitionDraft(k)?.link, 'external'))
      if (inner && e && n) {
        const to = `${ext}externals/${n}/`
        for (const p of this.sourcePaths) {
          if (p.startsWith(inner) && !isDocument(p.slice(inner.length))) files.set(to + p.slice(inner.length), await this.sourceText(p).catch(() => ''))
        }
        moved.set(to, cloneExternal(e))
      }
    }
    this.edit((x) => {
      let cx = 0, cy = 0
      for (const k of keys) {
        const p = x.position(k)
        cx += p.x
        cy += p.y
      }
      cx /= keys.size
      cy /= keys.size
      const inner: J[] = [], innerConns: J[] = []
      const inputs: [string, J][] = [], outputs: [string, J][] = []
      x.updateGraph(scope, (g) => {
        const outer: J[] = [], outerConns: J[] = []
        for (const c of arr(g.get('cards')) ?? []) (ids.has(str(get(c, 'id')) ?? '') ? inner : outer).push(c)
        const portName = (list: [string, J][], card: string, port: string) => {
          let n = `${card}_${port}`
          for (let i = 2; list.some(([nn]) => nn === n); i++) n = `${card}_${port}${i}`
          return n
        }
        for (const c of arr(g.get('connections')) ?? []) {
          const f = str(get(c, 'from', 'card')) ?? '', t = str(get(c, 'to', 'card')) ?? ''
          const fp = str(get(c, 'from', 'port')) ?? '', tp = str(get(c, 'to', 'port')) ?? ''
          const fi = ids.has(f), ti = ids.has(t)
          if (fi && ti) innerConns.push(c)
          else if (!fi && !ti) outerConns.push(c)
          else if (!fi && ti) {
            // One boundary input per inner input port (several wires may share it).
            const existing = inputs.find(([, v]) => str(get(v, 'card')) === t && str(get(v, 'port')) === tp)?.[0]
            const n = existing ?? portName(inputs, t, tp)
            if (!existing) inputs.push([n, obj([['card', t], ['port', tp]])])
            const nc = clone(c) as JObj
            nc.set('to', obj([['card', newId], ['port', n]]))
            outerConns.push(nc)
          } else {
            const existing = outputs.find(([, v]) => str(get(v, 'card')) === f && str(get(v, 'port')) === fp)?.[0]
            const n = existing ?? portName(outputs, f, fp)
            if (!existing) outputs.push([n, obj([['card', f], ['port', fp]])])
            const nc = clone(c) as JObj
            nc.set('from', obj([['card', newId], ['port', n]]))
            outerConns.push(nc)
          }
        }
        outer.push(obj([['id', newId]]))
        g.set('cards', outer)
        g.set('connections', outerConns)
      })
      // The external: a project of its own (it also opens on its own).
      const root = scopePrefix === '' ? x.project : x.external(scopePrefix)?.project ?? obj()
      const e: ExternalProject = {
        project: obj([
          ['name', newId], ['pkgsPath', clone(get(root, 'pkgsPath')) ?? 'tsugumi:default'], ['databaseConnections', []],
          ['canvas', obj([['cards', inner], ['connections', innerConns]])],
          ['boundary', obj([['inputs', new JObj(inputs)], ['outputs', new JObj(outputs)]])],
        ]),
        layout: obj(), definitions: new Map(),
      }
      const extCards: [string, J][] = []
      for (const k of [...keys].sort()) {
        const id = path(k).pop()
        if (!id) continue
        const p = x.position(k)
        extCards.push([id, obj([['x', real(p.x - cx)], ['y', real(p.y - cy)]])])
        const d = x.cardDefinitionDraft(k)
        if (d) e.definitions.set(id, cloneDef(d))
      }
      e.layout = obj([['cards', new JObj(extCards)]])
      for (const k of keys) {
        const inn = x.cardExternal(k)
        if (inn) x.removeExternal(inn)
        x.removeCardDefinition(k)
      }
      for (const [p, t] of files) {
        x.sourceRemovals.delete(p)
        x.sourceEdits.set(p, t)
      }
      for (const [to, inn] of moved) x.createExternal(to, inn)
      x.createExternal(ext, e)
      x.setCardDefinition(newKey, { card: obj(), link: obj([['external', newId]]) })
      x.updateLayout(scope, (l) => {
        const cards = l.get('cards')
        if (isObj(cards)) for (const kk of cards.keys()) if ([...ids].some((id) => kk === id || kk.startsWith(id + '/'))) cards.set(kk, undefined)
      })
      x.setPosition(newKey, { x: cx, y: cy })
      x.updateGroups(scope, (groups) => {
        for (const g of groups) if (isObj(g)) g.set('cards', (arr(g.get('cards')) ?? []).filter((c) => !ids.has(str(c) ?? '')))
      })
    })
    return newKey
  }

  // ---- Layout: notes, groups, output frames --------------------------------------------

  notes(scope: string[]): J[] {
    return (arr(get(this.layoutOf(scope), 'notes')) ?? []).filter((n) => (str(get(n, 'scope')) ?? '') === '')
  }

  addNote(scope: string[], p: { x: number; y: number }): string {
    const id = 'note-' + uuid().slice(0, 8)
    this.edit((x) => {
      x.updateLayout(scope, (l) => {
        l.set('notes', [...(arr(l.get('notes')) ?? []), obj([
          ['id', id], ['scope', ''], ['text', 'メモ'], ['color', 'yellow'],
          ['x', real(p.x)], ['y', real(p.y)], ['width', real(240)], ['height', real(120)],
        ])])
      })
    })
    return id
  }

  /** The scope whose layout holds a note or group (ids are unique). */
  private scopeHolding(field: string, id: string): string[] | undefined {
    const has = (l: J) => (arr(get(l, field)) ?? []).some((x) => str(get(x, 'id')) === id)
    if (has(this.layout)) return []
    const queue = this.childScopes([])
    while (queue.length) {
      const sc = queue.shift()!
      if (has(this.layoutOf(sc))) return sc
      queue.push(...this.childScopes(sc))
    }
    return undefined
  }

  note(id: string): JObj | undefined {
    const sc = this.scopeHolding('notes', id)
    if (!sc) return undefined
    const n = (arr(get(this.layoutOf(sc), 'notes')) ?? []).find((n) => str(get(n, 'id')) === id)
    return isObj(n) ? n : undefined
  }

  updateNote(id: string, fn: (n: JObj) => void) {
    this.edit((x) => x.changeNote(id, fn))
  }

  /** A step of an edit: see updateNote. */
  changeNote(id: string, fn: (n: JObj) => void) {
    const sc = this.scopeHolding('notes', id)
    if (!sc) return
    this.updateLayout(sc, (l) => {
      const notes = (arr(l.get('notes')) ?? []).map((n) => clone(n))
      const n = notes.find((n) => str(get(n, 'id')) === id)
      if (!isObj(n)) return
      fn(n)
      l.set('notes', notes)
    })
  }

  removeNote(id: string) {
    this.edit((x) => x.dropNote(id))
  }

  /** A step of an edit: see removeNote. */
  dropNote(id: string) {
    const sc = this.scopeHolding('notes', id)
    if (!sc) return
    this.updateLayout(sc, (l) => l.set('notes', (arr(l.get('notes')) ?? []).filter((n) => str(get(n, 'id')) !== id)))
  }

  groups(scope: string[]): JObj[] {
    return (arr(get(this.layoutOf(scope), 'groups')) ?? []).filter((g): g is JObj => isObj(g) && (str(g.get('scope')) ?? '') === '')
  }

  updateGroups(scope: string[], fn: (groups: J[]) => void) {
    this.updateLayout(scope, (l) => {
      const groups = (arr(l.get('groups')) ?? []).map((g) => clone(g))
      fn(groups)
      l.set('groups', groups.length ? groups : undefined)
    })
  }

  addGroup(keys: Set<CardKey>, scope: string[], name = 'グループ'): string {
    const id = 'group-' + uuid().slice(0, 8)
    this.edit((x) => x.updateGroups(scope, (g) => g.push(obj([
      ['id', id], ['scope', ''], ['name', name], ['color', 'blue'], ['cards', [...keys].map((k) => path(k).pop()!).sort()],
    ]))))
    return id
  }

  updateGroup(id: string, fn: (g: JObj) => void) {
    const sc = this.scopeHolding('groups', id)
    if (!sc) return
    this.edit((x) => x.updateGroups(sc, (groups) => {
      const g = groups.find((g) => str(get(g, 'id')) === id)
      if (isObj(g)) fn(g)
    }))
  }

  /** Removes the group frame only; its Cards stay where they are. */
  ungroup(id: string) {
    const sc = this.scopeHolding('groups', id)
    if (!sc) return
    this.edit((x) => x.updateGroups(sc, (groups) => {
      const i = groups.findIndex((g) => str(get(g, 'id')) === id)
      if (i >= 0) groups.splice(i, 1)
    }))
  }

  displayFrame(k: CardKey): J | undefined {
    return get(this.layoutOf(path(k).slice(0, -1)), 'displays', this.layoutKey(k))
  }

  /** A frame without a place of its own is drawn beside its Card, so it goes where the Card goes. */
  frameFollowsCard(k: CardKey, moving: Set<CardKey>): boolean {
    return moving.has(k) && dbl(get(this.displayFrame(k), 'x')) === undefined
  }

  updateDisplayFrame(k: CardKey, fn: (d: JObj) => void) {
    this.edit((x) => x.changeDisplayFrame(k, fn))
  }

  /** A step of an edit: see updateDisplayFrame. */
  changeDisplayFrame(k: CardKey, fn: (d: JObj) => void) {
    const lk = this.layoutKey(k)
    this.updateLayout(path(k).slice(0, -1), (l) => {
      let displays = l.get('displays')
      if (!isObj(displays)) {
        displays = obj()
        l.set('displays', displays)
      }
      let d = (displays as JObj).get(lk)
      if (!isObj(d)) {
        d = obj()
        ;(displays as JObj).set(lk, d)
      }
      fn(d as JObj)
    })
  }

  /** Where a scope's boundary ports are drawn (inside a Card in Card). */
  boundaryPosition(scope: string[], side: string): { x: number; y: number } | undefined {
    const v = get(this.layoutOf(scope), 'boundary', side)
    const x = dbl(get(v, 'x')), y = dbl(get(v, 'y'))
    return x !== undefined && y !== undefined ? { x, y } : undefined
  }

  setBoundaryPosition(scope: string[], side: string, p: { x: number; y: number }) {
    this.edit((x) => x.updateLayout(scope, (l) => {
      let b = l.get('boundary')
      if (!isObj(b)) {
        b = obj()
        l.set('boundary', b)
      }
      ;(b as JObj).set(side, obj([['x', real(Math.round(p.x * 10) / 10)], ['y', real(Math.round(p.y * 10) / 10)]]))
    }))
  }

  /** The direction this project's Cards are laid out in (layout.json "flow");
   *  without one, read from where the wired Cards sit, else the app's default. */
  get flow(): FlowDirection {
    const f = str(get(this.layout, 'flow'))
    if (f === 'topToBottom' || f === 'leftToRight') return f
    let across = 0, down = 0
    for (const c of this.connections([])) {
      const f2 = str(get(c, 'from', 'card')), t = str(get(c, 'to', 'card'))
      if (!f2 || !t) continue
      const a = this.position(f2), b = this.position(t)
      across += Math.abs(b.x - a.x)
      down += Math.abs(b.y - a.y)
    }
    if (across + down > 0) return across > down ? 'leftToRight' : 'topToBottom'
    return prefs.flowDirection
  }

  setFlow(f: FlowDirection) {
    this.edit((x) => {
      if (!isObj(x.layout)) x.layout = obj()
      ;(x.layout as JObj).set('flow', f)
    })
  }

  /** Lays out a scope's Cards, output frames and notes (layout only, one undo):
   *  see `arrangeCards`. Sizes and port positions are those of `CardGeometry`. */
  arrange(scope: string[]) {
    const ids = this.cards(scope).map((c) => str(get(c, 'id'))).filter((x): x is string => !!x)
    const cards = ids.map((id) => {
      const g = this.geometry(key([...scope, id]))
      const ports = (names: string[], output: boolean) => names.map((name) => ({ name, ...g.port(name, output) }))
      return { id, ...g.size, inputs: ports(g.inputs, false), outputs: ports(g.outputs, true) }
    })
    const wires = this.connections(scope).flatMap((c) => {
      const from = str(get(c, 'from', 'card')), fromPort = str(get(c, 'from', 'port')), to = str(get(c, 'to', 'card')), toPort = str(get(c, 'to', 'port'))
      return from && fromPort && to && toPort ? [{ from, fromPort, to, toPort }] : []
    })
    const frames = ids.flatMap((id) => {
      const k = key([...scope, id]), f = this.displayFrame(k)
      if ((!this.displays.has(k) && f === undefined) || bool(get(f, 'hidden')) === true) return []
      return [{ id, width: dbl(get(f, 'width')) ?? 320, height: dbl(get(f, 'height')) ?? 170 }]
    })
    const notes = this.notes(scope).flatMap((n) => {
      const id = str(get(n, 'id'))
      return id ? [{ id, width: dbl(get(n, 'width')) ?? 240, height: dbl(get(n, 'height')) ?? 120 }] : []
    })
    const bar = (side: string) => {
      const list = scope.length ? get(this.boundary(key(scope)), side) : undefined
      return isObj(list) ? list.entries().map(([, v]) => ({ card: str(get(v, 'card')) ?? '', port: str(get(v, 'port')) ?? '' })) : []
    }
    const placed = arrangeCards({ flow: this.flow, grid: prefs.gridSize, snap: prefs.showGrid, cards, wires, frames, notes, boundary: { inputs: bar('inputs'), outputs: bar('outputs') } })
    this.edit((x) => {
      for (const c of placed.cards) x.setPosition(key([...scope, c.id]), c)
      x.updateLayout(scope, (l) => {
        if (placed.frames.length) {
          let displays = l.get('displays')
          if (!isObj(displays)) {
            displays = obj()
            l.set('displays', displays)
          }
          for (const f of placed.frames) {
            const lk = this.layoutKey(key([...scope, f.id]))
            let d = (displays as JObj).get(lk)
            if (!isObj(d)) {
              d = obj()
              ;(displays as JObj).set(lk, d)
            }
            ;(d as JObj).set('x', real(f.x))
            ;(d as JObj).set('y', real(f.y))
          }
        }
        if (placed.notes.length) {
          l.set('notes', (arr(l.get('notes')) ?? []).map((n) => {
            const at = placed.notes.find((p) => p.id === str(get(n, 'id')))
            const copy = clone(n)
            if (at && isObj(copy) && (str(get(n, 'scope')) ?? '') === '') {
              copy.set('x', real(at.x))
              copy.set('y', real(at.y))
            }
            return copy
          }))
        }
        // The bars of a Card in Card go back to following the Cards.
        l.set('boundary', undefined)
      })
    })
  }

  // ---- Definitions ---------------------------------------------------------------

  removeDefinition(id: string) {
    this.removeCardDefinition(key([id]))
  }

  /** Removes a Card's definition folder (its documents and every file in it). */
  removeCardDefinition(k: CardKey) {
    const dir = this.definitionDir(k)
    if (!dir) return
    this.setCardDefinition(k, undefined)
    const prefix = this.projectPrefix(path(k).slice(0, -1)) ?? ''
    for (const p of this.sourcePaths) {
      if (p.startsWith(dir) && !(prefix && isDocument(p.slice(prefix.length)))) this.removeSource(p)
    }
  }

  putSource(p: string, text: string) {
    this.sourceRemovals.delete(p)
    this.sourceEdits.set(p, text)
  }

  removeSource(p: string) {
    this.sourceEdits.delete(p)
    if (this.sources.has(p)) this.sourceRemovals.add(p)
  }

  /** The definition folder of a new Card made from a template (in the draft). */
  writeTemplate(t: DefinitionTemplate, k: CardKey) {
    const dir = this.definitionDir(k), id = path(k).pop()
    if (!dir || !id) return
    const ns = id.replace(/[-.]/g, '_')
    const set = (card: J, meta?: J) => this.setCardDefinition(k, { card, meta })
    switch (t) {
      case 'draft':
        set(obj([['template', 'draft']]), obj([['description', ''], ['content', '']]))
        break
      case 'trigger': case 'fork': case 'branch': case 'logger':
        set(obj([['template', t]]))
        break
      case 'merge':
        set(obj([['template', 'merge'], ['inputs', [obj([['name', 'a']]), obj([['name', 'b']])]]]))
        break
      case 'input':
        set(obj([['template', 'input'], ['params', [obj([['name', 'value'], ['type', 'double'], ['data', num('0')]])]]]))
        this.putSource(dir + '_outputs.fbs', `namespace ${ns};\ntable Value { value:double; }\ntable Outputs { value:Value (required); }\nroot_type Outputs;\n`)
        break
      case 'python':
        set(obj([['language', 'python'], ['inputs', [obj([['name', 'in']])]]]))
        this.putSource(dir + 'default.nix', '{ pkgs, inputs, params }:\nlet\n  sdk = inputs.tsugumi.sdk { languages = [ "python" ]; };\nin\n{\n  run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; };\n}\n')
        this.putSource(dir + '_outputs.fbs', `namespace ${ns};\ntable Text { value:string; }\ntable Outputs { out:Text (required); }\nroot_type Outputs;\n`)
        this.putSource(dir + 'src/main.py', `import outputs.out.${ns}.Text as out


def main(ctx):
    b = ctx.builder()
    text = b.CreateString("hello")
    out.Start(b)
    out.AddValue(b, text)
    ctx.output("out", b, out.End(b))
`)
        break
      case 'cpp':
        set(obj([['language', 'cpp'], ['inputs', [obj([['name', 'in']])]]]))
        this.putSource(dir + 'default.nix', '{ pkgs, inputs, params }:\nlet\n  sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };\nin\n{\n  run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };\n}\n')
        this.putSource(dir + '_outputs.fbs', `namespace ${ns};\ntable Text { value:string; }\ntable Outputs { out:Text (required); }\nroot_type Outputs;\n`)
        this.putSource(dir + 'src/card.cc', `#include <card.hh>

auto run(Card::Input& input) -> Card::Output {
  Card::Output out;
  out.out.emplace().value = "hello";
  return out;
}
`)
        break
    }
  }

  async clearBuildCache(): Promise<R.ProjectCacheClearResultT> {
    if (!this.client) throw new RPCError('disconnected', '接続していません')
    return this.client.clearCache(uuid())
  }

  // ---- Versions (git) ----------------------------------------------------------

  /** The project's versions as last read (core/src/versions.cc). */
  versionGraph?: R.VersionGraphT
  /** Made or copied by this page: it becomes a repository when it connects. */
  makeRepositoryOnConnect = false

  async refreshVersions() {
    if (!this.client) return
    try {
      this.versionGraph = await this.client.versionGraph()
    } catch {}
    this.changed()
  }

  /** A version's Cards and wires (a commit never changes: read once). */
  versionCanvases = new Map<string, R.VersionCanvasT>()
  versionSummaryBusy = new Set<string>()
  versionSummaryErrors = new Map<string, string>()
  private versionSummaryTried = new Set<string>()

  async loadVersionCanvas(commit: string) {
    if (!this.client || this.versionCanvases.has(commit)) return
    try {
      this.versionCanvases.set(commit, await this.client.versionCanvas(commit))
      this.changed()
    } catch {}
  }

  /** Switched in the version tree; the server keeps it and refuses while off. */
  versionSummaries = false

  async loadVersionSummaries() {
    const v = await api<{ versionSummaries?: boolean }>('/api/settings').catch(() => null)
    this.versionSummaries = !!v?.versionSummaries
    this.changed()
  }

  async setVersionSummaries(on: boolean) {
    await api('/api/settings', { versionSummaries: on })
    this.versionSummaries = on
    this.versionSummaryTried.clear()
    this.versionSummaryErrors.clear()
    this.changed()
  }

  /** Has a small model write what `commit` changed, once, while switched on:
   *  as a run of it ends and when it is hovered without a summary. Only its
   *  code and settings go out (VersionDiff); the sentence goes to its note. */
  async summarizeVersion(commit: string) {
    if (!this.client || this.versionSummaryTried.has(commit)) return
    await this.loadVersionSummaries()
    if (!this.versionSummaries || this.versionSummaryTried.has(commit)) return
    this.versionSummaryTried.add(commit)
    this.versionSummaryBusy.add(commit)
    this.changed()
    try {
      await this.refreshVersions()
      const c = this.versionGraph?.commits.find((x) => x && s(x.id) === commit)
      if (c && !c.changeSummary) {
        const diff = await this.client.versionDiff(commit)
        // Nothing a summary may read changed (the layout, data files).
        if (s(diff.text)) {
          const { text } = await api<{ text: string }>('/api/versions/summarize', { input: versionSummaryInput(c, diff) })
          await this.client.versionSummary(uuid(), 'ui', commit, text)
          await this.refreshVersions()
        }
      }
    } catch (e) {
      this.versionSummaryErrors.set(commit, String((e as Error)?.message ?? e))
    }
    this.versionSummaryBusy.delete(commit)
    this.changed()
  }

  async makeRepository() {
    if (!this.client) throw new RPCError('disconnected', '接続していません')
    await this.client.versionInit(uuid(), 'ui')
    await this.refreshVersions()
  }

  /** Names what is saved now as a version (a commit and an annotated tag). */
  async markVersion(name: string, message: string) {
    if (!this.client) throw new RPCError('disconnected', '接続していません')
    if (this.dirty) await this.save()
    if (this.dirty) throw new Error('未保存の変更があります。保存してから選んでください。')
    await this.client.versionMark(uuid(), 'ui', name, message)
    await this.refreshVersions()
  }

  /** Starts from `commit`: the documents become that version's; a change and
   *  run from there opens a branch, and nothing later is lost. */
  async startFrom(commit: string) {
    if (!this.client) throw new RPCError('disconnected', '接続していません')
    if (this.runActive) throw new Error('実行中は別の版に移れません。')
    if (this.dirty) throw new Error('未保存の変更があります。保存してから選んでください。')
    await this.client.versionCheckout(this.rev, uuid(), 'ui', commit)
    await this.refreshVersions()
  }

  async deleteRunHistory(endedBefore: Date | null): Promise<R.RunHistoryDeleteResultT> {
    if (!this.client) throw new RPCError('disconnected', '接続していません')
    return this.client.deleteHistory(uuid(), endedBefore ? endedBefore.getTime() : null)
  }

  // ---- Canvas data (CardNode) ---------------------------------------------------------

  /** Ports for drawing: resolved ports when available, else declared inputs. */
  ports(k: CardKey): { inputs: string[]; outputs: string[] } {
    const d = this.portData(k)
    return { inputs: d.inputs.map((p) => p.name), outputs: d.outputs.map((p) => p.name) }
  }

  portData(k: CardKey, definition?: J, meta?: J): { inputs: PortData[]; outputs: PortData[] } {
    const def = definition ?? this.cardDefinition(k)
    const m = meta ?? this.cardMeta(k)
    const note = (list: string, name: string) => str(get((arr(get(m, list)) ?? []).find((x) => str(get(x, 'name')) === name), 'description'))
    const r = this.resolution.get(k)
    // A Draft's ports change with its wires, before they are resolved again.
    const draft = this.kind(k) === 'draft'
    if (definition === undefined && r && !draft) {
      return {
        inputs: r.inputs.map((p) => ({ name: p.name, typeName: p.rootType, optional: p.optional, note: note('inputs', p.name) })),
        outputs: r.outputs.map((p) => ({ name: p.name, typeName: p.rootType, optional: p.optional, note: note('outputs', p.name) })),
      }
    }
    // Not resolved yet (unsaved) or a working copy: declared ports.
    const ins = (arr(get(def, 'inputs')) ?? []).flatMap((i) => {
      const n = str(get(i, 'name'))
      return n ? [{ name: n, typeName: r?.inputs.find((p) => p.name === n)?.rootType, optional: bool(get(i, 'optional')) ?? false, note: note('inputs', n) }] : []
    })
    // A Draft's resolved outputs beyond the declared ones are those of its _outputs.fbs.
    const dir = draft ? this.definitionDir(k) : undefined
    const typed = !draft || (dir !== undefined && this.hasSource(dir + '_outputs.fbs'))
    const outs: PortData[] = (typed ? r?.outputs ?? [] : []).map((p) => ({ name: p.name, typeName: p.rootType, optional: p.optional, note: note('outputs', p.name) }))
    if (draft || !outs.length) {
      for (const o of arr(get(m, 'outputs')) ?? []) {
        const n = str(get(o, 'name'))
        if (n && !outs.some((p) => p.name === n)) outs.push({ name: n, typeName: r?.outputs.find((p) => p.name === n)?.rootType, note: str(get(o, 'description')) })
      }
    }
    return { inputs: ins, outputs: outs }
  }

  /** Params edited directly on input Cards. */
  inlineParams(k: CardKey, definition?: J): ParamData[] {
    const def = definition ?? this.cardDefinition(k)
    if (this.kind(k) !== 'input' || def === undefined) return []
    return (arr(get(def, 'params')) ?? []).flatMap((p) => {
      const name = str(get(p, 'name')), type = str(get(p, 'type'))
      if (!name || !type || !['int64', 'uint64', 'double', 'bool', 'string'].includes(type)) return []
      return [{ name, type, value: paramText(get(p, 'data')) }]
    })
  }

  cardDescription(k: CardKey): string | undefined {
    return str(get(this.cardMeta(k), 'description')) || undefined
  }

  /** What a Card says about itself on hover: a Draft's summary and content. */
  cardDetails(k: CardKey): string[] {
    const desc = this.cardDescription(k)
    const content = this.kind(k) === 'draft' ? str(get(this.cardMeta(k), 'content')) : undefined
    return [desc, content].filter((x): x is string => !!x)
  }

  /** Whether a Card does not run because it needs a Draft's output. */
  waitsOnDraft(k: CardKey): boolean {
    return this.draftWaiting(path(k).slice(0, -1)).has(path(k).pop() ?? k)
  }

  /** Card ids of a scope that do not run because of a Draft. */
  draftWaiting(scope: string[]): Set<string> {
    const ids = this.cards(scope).map((c) => str(get(c, 'id'))).filter((x): x is string => !!x)
    const waits = new Set(ids.filter((id) => this.kind(key([...scope, id])) === 'draft'))
    if (!waits.size) return waits
    const conns = this.connections(scope)
    let changed = true
    while (changed) {
      changed = false
      for (const id of ids) {
        if (waits.has(id)) continue
        for (const p of this.resolution.get(key([...scope, id]))?.inputs ?? []) {
          if (p.optional) continue
          const from = conns.filter((c) => str(get(c, 'to', 'card')) === id && str(get(c, 'to', 'port')) === p.name).map((c) => str(get(c, 'from', 'card')) ?? '')
          if (from.length && from.every((f) => waits.has(f))) {
            waits.add(id)
            changed = true
            break
          }
        }
      }
    }
    return waits
  }

  /** One line describing the latest result of a Card. */
  resultSummary(k: CardKey): string | undefined {
    const st = this.cardStates.get(k)
    if (st && st.state === D.CardState.failed) return st.failure ?? '失敗'
    if (st && st.state === D.CardState.blocked) return st.failure ?? '入力が届きません'
    const d = this.displays.get(k)
    return d ? displaySummary(d) : undefined
  }

  /** Stage strip of the latest invocation (undefined before any run of the Card). */
  stageProgress(k: CardKey): StageProgress | undefined {
    const st = this.cardStates.get(k)
    if (!st || st.invocation <= 0n) return undefined
    const ended = ENDED.includes(st.state)
    const items = applicableStages(this.kind(k) === 'code').map((shown) => {
      const parts: StageStatus[] = []
      let seconds: number | undefined
      for (const sp of stageParts(shown)) {
        const r = [...st.stages].reverse().find((x) => x.stage === sp)
        if (!r) {
          parts.push(ended ? 'notRun' : 'pending')
          continue
        }
        switch (r.outcome) {
          case D.StageOutcome.running: parts.push(ended ? 'notRun' : 'running'); break
          case D.StageOutcome.cached: parts.push('cached'); break
          case D.StageOutcome.reused: parts.push('reused'); break
          case D.StageOutcome.failed: parts.push('failed'); break
          case D.StageOutcome.cancelled: parts.push('cancelled'); break
          default: parts.push('done')
        }
        if (r.end !== undefined && r.start !== undefined) seconds = (seconds ?? 0) + Math.max(0, r.end - r.start) / 1000
      }
      return { stage: shown, status: mergeStageStatus(parts, ended), seconds }
    })
    return { items, workerReused: st.workerReused }
  }

  /** State of a Card in Card from the Cards inside it. */
  compositeState(k: CardKey): D.CardState | undefined {
    const inner = [...this.cardStates].filter(([kk]) => kk.startsWith(k + '/')).map(([, v]) => v.state)
    if (!inner.length) return undefined
    if (inner.includes(D.CardState.failed)) return D.CardState.failed
    if (inner.includes(D.CardState.cancelled)) return D.CardState.cancelled
    if (inner.some((x) => x === D.CardState.running || x === D.CardState.preparing)) return D.CardState.running
    if (this.runActive && inner.some((x) => x === D.CardState.waiting || x === D.CardState.ready)) return D.CardState.running
    if (inner.includes(D.CardState.blocked)) return D.CardState.blocked
    return D.CardState.succeeded
  }

  /** The inner Card and port an outer output of a Card in Card comes from. */
  boundarySource(k: CardKey, port: string): [CardKey, string] | undefined {
    const ep = get(this.boundary(k), 'outputs', port)
    const c = str(get(ep, 'card')), p = str(get(ep, 'port'))
    return c && p ? [key([...path(k), c]), p] : undefined
  }

  nodeData(k: CardKey, opts: { definition?: J; meta?: J; collapsed?: boolean; outputs?: PortData[] } = {}): CardNodeData {
    const kind = this.kind(k)
    const state = this.cardStates.get(k)
    const diags = this.resolution.get(k)?.diagnostics ?? []
    const ports = this.portData(k, opts.definition, opts.meta)
    const def = opts.definition ?? this.cardDefinition(k)
    const m = opts.meta ?? this.cardMeta(k)
    let summary = str(get(m, 'description'))
    // What a Draft shows on its Card is what it says it will do.
    if (kind === 'draft') summary = str(get(m, 'content')) || summary
    const frame = this.displayFrame(k)
    const display = (this.displays.has(k) || frame !== undefined) && bool(get(frame, 'hidden')) !== true
    const d = this.displays.get(k)
    const sym = str(get(def, 'symbol'))
    const editing = kind === 'draft' && this.inlineDraft?.card === k
    return {
      title: path(k).pop() ?? k, kind, symbol: sym || undefined, summary: summary || undefined,
      inputs: ports.inputs, outputs: opts.outputs ?? ports.outputs, params: this.inlineParams(k, opts.definition),
      result: d ? displaySummary(d) : !state && kind !== 'draft' && this.waitsOnDraft(k) ? 'Draft のため実行しない' : undefined,
      state: kind === 'subgraph' ? this.compositeState(k) : state?.state, stage: this.stageProgress(k),
      invocation: state?.invocation ?? 0n, runContinues: this.runActive,
      failure: state && (state.state === D.CardState.failed || state.state === D.CardState.blocked) ? state.failure ?? cardState(state.state).label : undefined,
      errors: diags.filter((x) => x.error).map((x) => x.message), warnings: diags.filter((x) => !x.error).map((x) => x.message),
      collapsed: !editing && (opts.collapsed ?? this.collapsed(k)), hasDisplay: display, permissions: cardPermissions(def),
      draftEditing: editing, untitled: this.draftUntitled(k),
      portGrid: prefs.showGrid ? prefs.gridSize : undefined,
    }
  }

  /** Where a Card is drawn: with the grid on, one saved off the grid (placed
   *  before the grid was on, or under another spacing) is shown on the nearest
   *  line, so its ports are on the grid too. Nothing is saved until it is moved. */
  placedPosition(k: CardKey): Pt {
    const p = this.position(k)
    return prefs.showGrid ? { x: snapToGrid(p.x, prefs.gridSize), y: snapToGrid(p.y, prefs.gridSize) } : p
  }

  geometry(k: CardKey): CardGeometry {
    return new CardGeometry(this.nodeData(k), this.flow)
  }
}

/** An external's documents and files, ready to be written under another folder. */
export class ExternalCopy {
  docs = new Map<string, ExternalProject>() // folder -> documents (the external and the ones in it)
  files = new Map<string, string>()

  apply(sess: ProjectSession) {
    for (const [p, t] of this.files) {
      sess.sourceRemovals.delete(p)
      sess.sourceEdits.set(p, t)
    }
    for (const [p, e] of [...this.docs].sort(([a], [b]) => a.localeCompare(b))) sess.createExternal(p, e)
  }
}

// ---- Kinds, templates and labels ------------------------------------------------------

/** What a Card is, for choosing what the canvas and the editor offer. */
export type CardKind = 'trigger' | 'input' | 'fork' | 'branch' | 'merge' | 'logger' | 'draft' | 'subgraph' | 'code'
const KINDS: CardKind[] = ['trigger', 'input', 'fork', 'branch', 'merge', 'logger', 'draft', 'subgraph', 'code']

export const KIND_LABEL: Record<CardKind, string> = {
  trigger: '開始', input: '入力', fork: '分配', branch: '分岐', merge: '合流', logger: '記録', draft: 'Draft', subgraph: 'Card in Card', code: '処理',
}

export const KIND_ICON: Record<CardKind, string> = {
  trigger: 'play.circle', input: 'slider.horizontal.3', fork: 'arrow.triangle.branch', branch: 'arrow.triangle.swap',
  merge: 'arrow.triangle.merge', logger: 'list.bullet.rectangle', draft: 'pencil.and.outline', subgraph: 'link',
  code: 'chevron.left.forwardslash.chevron.right',
}

export function definitionKind(def: J | undefined): CardKind {
  const t = str(get(def, 'template'))
  if (t && (KINDS as string[]).includes(t)) return t as CardKind
  return get(def, 'language') === undefined ? 'draft' : 'code'
}

/** What a new Card starts from: a built-in Card or a scaffold. Every Card
 *  gets a definition folder of its own (definitions/<id>/). */
export type DefinitionTemplate = 'python' | 'cpp' | 'input' | 'draft' | 'trigger' | 'fork' | 'branch' | 'merge' | 'logger'
export const TEMPLATES: { id: DefinitionTemplate; label: string }[] = [
  { id: 'python', label: 'Python の処理' }, { id: 'cpp', label: 'C++ の処理' }, { id: 'input', label: '入力値' },
  { id: 'draft', label: 'Draft（未実装の計画）' }, { id: 'trigger', label: '開始' }, { id: 'fork', label: '分配' },
  { id: 'branch', label: '分岐' }, { id: 'merge', label: '合流' }, { id: 'logger', label: '記録' },
]
export const templateIcon = (t: DefinitionTemplate) => KIND_ICON[t === 'python' || t === 'cpp' ? 'code' : (t as CardKind)]
/** A name for the new Card (its id; made unique). */
export const templateBaseName = (t: DefinitionTemplate) => (t === 'python' || t === 'cpp' ? 'card' : t)

export const NOTE_COLORS: { id: string; label: string }[] = [
  { id: 'yellow', label: '黄' }, { id: 'blue', label: '青' }, { id: 'green', label: '緑' }, { id: 'pink', label: '桃' }, { id: 'gray', label: '灰' },
]

export const RUN_STATE: Record<number, { label: string; icon: string; color: string }> = {
  [D.RunState.idle]: { label: '待機中', icon: 'circle', color: 'secondary' },
  [D.RunState.queued]: { label: '待機列', icon: 'hourglass', color: 'orange' },
  [D.RunState.preparing]: { label: '準備中', icon: 'hourglass', color: 'orange' },
  [D.RunState.running]: { label: '実行中', icon: 'play.circle.fill', color: 'orange' },
  [D.RunState.cancelling]: { label: '停止中', icon: 'stop.circle', color: 'orange' },
  [D.RunState.succeeded]: { label: '成功', icon: 'checkmark.circle.fill', color: 'green' },
  [D.RunState.failed]: { label: '失敗', icon: 'xmark.circle.fill', color: 'red' },
  [D.RunState.cancelled]: { label: '停止済み', icon: 'stop.circle.fill', color: 'brown' },
}
export const runState = (st: D.RunState | undefined) => RUN_STATE[st ?? D.RunState.idle] ?? { label: '未実行', icon: 'circle', color: 'secondary' }

export const CARD_STATE: Record<number, { label: string; icon: string; color: string }> = {
  [D.CardState.not_started]: { label: '未実行', icon: 'circle.dotted', color: 'secondary' },
  [D.CardState.waiting]: { label: '入力待ち', icon: 'hourglass', color: 'orange' },
  [D.CardState.ready]: { label: '実行待ち', icon: 'hourglass', color: 'orange' },
  [D.CardState.preparing]: { label: '準備中', icon: 'gearshape', color: 'orange' },
  [D.CardState.running]: { label: '実行中', icon: 'play.circle.fill', color: 'orange' },
  [D.CardState.succeeded]: { label: '成功', icon: 'checkmark.circle.fill', color: 'green' },
  [D.CardState.failed]: { label: '失敗', icon: 'xmark.circle.fill', color: 'red' },
  [D.CardState.blocked]: { label: 'ブロック', icon: 'nosign', color: 'purple' },
  [D.CardState.cancelled]: { label: '停止済み', icon: 'stop.circle.fill', color: 'brown' },
  [D.CardState.skipped]: { label: 'スキップ', icon: 'arrow.uturn.right.circle', color: 'gray' },
}
export const cardState = (st: D.CardState) => CARD_STATE[st] ?? { label: '不明', icon: 'circle.dotted', color: 'secondary' }

export function formatMillis(ms: bigint | number | null | undefined): string {
  if (ms === null || ms === undefined) return ''
  return new Date(Number(ms)).toLocaleString('ja-JP', { year: 'numeric', month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

/** One form everywhere (run, stages, timeline, history): hh:mm:ss.xx. */
export function formatDuration(seconds: number): string {
  const cs = Math.floor(Math.max(0, seconds) * 100)
  const two = (n: number) => String(n).padStart(2, '0')
  return `${two(Math.floor(cs / 360_000))}:${two(Math.floor(cs / 6000) % 60)}:${two(Math.floor(cs / 100) % 60)}.${two(cs % 100)}`
}

export function formatBytes(n: number | bigint | null | undefined): string {
  const v = Number(n ?? 0)
  if (v < 1000) return `${v} bytes`
  const units = ['KB', 'MB', 'GB', 'TB']
  let x = v / 1000, i = 0
  while (x >= 1000 && i < units.length - 1) {
    x /= 1000
    i++
  }
  return `${x.toFixed(x < 10 ? 1 : 0)} ${units[i]}`
}

/** Integers keep their exact decimal spelling (64 bit safe); the core
 *  validates types and ranges and reports what it rejects. */
export function parseParam(text: string, type: string): J {
  const t = text.trim()
  switch (type) {
    case 'int64': case 'uint64': return /^-?[0-9]+$/.test(t) ? num(t) : t
    case 'double': return t !== '' && /^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?$/.test(t) ? num(t) : t
    case 'bool': return t === 'true'
    case 'string': return text
    default: return tryParse(t) ?? text
  }
}

export const paramText = (v: J | undefined): string => {
  if (typeof v === 'string') return v
  const n = numText(v)
  if (n !== undefined) return n
  if (typeof v === 'boolean') return v ? 'true' : 'false'
  if (Array.isArray(v)) return serialize(v, false)
  return ''
}
