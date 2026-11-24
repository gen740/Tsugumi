import { DocumentRevisions } from './documentRevisions'
import { projectOperation, operationInverse } from './projectOperation'
import { connectionsOf, SavePresence } from './presence'
import { readViewState, writeViewState } from '../../shared/clientViewState'
import { layoutHost, H, sceneRequest } from '../wire/hostLayout'
import { VersionCommentT } from '../../shared/gen/tsugumi/rpc/version-comment'
import { RevisionEdit } from '../ui/revisionEdit'
import { LiveViews } from './liveViews'
// One open project: shared by every view showing it. Owns the RPC
// connection, the confirmed document (from tsugumid), the local draft with
// undo/redo, run progress, displays and logs. Every edit is saved at once:
// the draft holds only what is on its way to the core, or what could not be
// saved (a run in progress, a lost connection).
import { api, onServerEvent } from '../api'
import { projectHost } from '../wire/projectHost'
import { hostCall, onHostEvent } from '../wire/host'
import { environmentHost, environmentProgress, environmentState, type EnvPublicState } from '../wire/environment'
import { parse, arr, bool, clone, dbl, get, isObj, J, jeq, JObj, numText, obj, real, serialize, str, tryParse } from '../../shared/json'
import { cardPath, D, R, LiveDisplayUpdate, RPCClient, RPCError, RPCFailure, s, segs, sOpt, Transport, uuid, webSocketTransport } from '../wire/rpc'
import {
  applicableStages, CardGeometry, CardNodeData, mergeStageStatus, orderedPorts, ParamData, PortData, Pt, Stage, stageOfPhase, stageParts, StageProgress, StageStatus,
  snapToGrid,
} from './canvasKit'
import type { LayoutBar, LayoutScene } from '../../shared/layoutScene'
import { cloneDef, cloneExternal, defEq } from './documentValues'
import { cardNotes, layoutView, noteId, parseReadme, printReadme, splitCardNotes } from './documents'
import { Observable } from './observable'
import { FlowDirection, prefs } from './prefs'
import { runMark } from './runMark'
import { cardPermissions } from './permissions'
import { L } from '../i18n'

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
  rootType?: string
  mergeMemberTypes?: string[]
  executionShape?: D.ExecutionShape
}

export function portTypeName(port?: PortInfo): string | undefined {
  if (port?.mergeMemberTypes?.length) return port.mergeMemberTypes.join(' | ')
  return port?.rootType
}

export interface CardParameter {
  name: string
  type: string
  value: J
}

function parameterLiteral(value: J, type: string): string {
  if (Array.isArray(value) && type.startsWith('[')) {
    const items = value.map(item => parameterLiteral(item, type.slice(1, -1)))
    return '[ ' + items.map(item => item.startsWith('-') ? '(' + item + ')' : item).join(' ') + ' ]'
  }
  if (typeof value === 'string') {
    return '"' + value.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\$\{/g, '\\$' + '{') + '"'
  }
  const literal = serialize(value, false)
  if (type === 'float') return literal.replace(/^(-?\d+)(?=[eE]|$)/, '$1.0')
  return literal
}

export interface ResolvedCard {
  path: string[]
  inputs: PortInfo[]
  outputs: PortInfo[]
  diagnostics: Diagnostic[]
  nixReferences: { target: string[]; name: string }[]
  parameters?: CardParameter[]
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

/** What the user and the Agent write about a Card (_comments.json), and the record
 *  of who did what ("agent" or "user"; absent when not recorded). */
export interface CardComment {
  id: string
  text: string
  resolved: boolean
  finding?: boolean
  /** One line on what was done about it. */
  resolution?: string
  author?: string
  editedBy?: string
  stateBy?: string
  /** The Cards the save that resolved it changed. */
  cards: string[]
}

export function cardComments(meta: J | undefined): CardComment[] {
  return (arr(get(meta, 'comments')) ?? []).flatMap((c) => {
    const id = str(get(c, 'id')), text = str(get(c, 'text'))
    return id && text !== undefined ? [{
      id, text, resolved: bool(get(c, 'resolved')) ?? false, finding: bool(get(c, 'finding')) ?? false, resolution: str(get(c, 'resolution')) || undefined,
      author: str(get(c, 'author')), editedBy: str(get(c, 'edited_by')), stateBy: str(get(c, 'state_by')),
      cards: (arr(get(c, 'cards')) ?? []).map((x) => str(x)).filter((x): x is string => !!x),
    }] : []
  })
}

/** Who did something to a comment, for the sentences that say so. */
export const commentActor = (by: string | undefined) => (by === 'agent' ? 'Agent' : by === 'user' ? L('あなた') : undefined)
/** "Resolved by the Agent", or undefined when who did it is not recorded. */
export const commentDone = (by: string | undefined, what: string) => (by === 'agent' ? L('Agent が{0}', what) : by === 'user' ? L('あなたが{0}', what) : undefined)

/** What a version or a save did to a comment, and who did it when that is recorded. */
export function commentChange(change: string, by?: string): string {
  const what = ({ added: L('追加'), edited: L('編集'), resolved: L('解決'), reopened: L('開き直し'), removed: L('削除') } as Record<string, string>)[change] ?? change
  const who = commentActor(by)
  return who ? L('{0}（{1}）', what, who) : what
}

/** Latest invocation of a Card in the current run, as the core reports it. *//** Latest invocation of a Card in the current run, as the core reports it. */
export interface CardRunState {
  state: D.CardState
  /** Number of the latest invocation in this run (1, 2, …); 0 = not run. */
  invocation: bigint
  phase: D.Phase
  failure?: string
  executionState?: D.RemoteExecutionState | null
  transfer?: string
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

export function remoteExecutionLabel(state: D.RemoteExecutionState | null | undefined): string | undefined {
  switch (state) {
    case D.RemoteExecutionState.queued: return L('実行サービスで待機中')
    case D.RemoteExecutionState.running: return L('実行中')
    case D.RemoteExecutionState.disconnected: return L('接続断（結果未確認）')
    case D.RemoteExecutionState.cancelling: return L('取り消し確認中')
    case D.RemoteExecutionState.succeeded: return L('成功')
    case D.RemoteExecutionState.failed: return L('失敗')
    case D.RemoteExecutionState.cancelled: return L('停止済み')
    case D.RemoteExecutionState.interrupted: return L('中断')
    default: return undefined
  }
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

/** A Draft being written on its Card: its content as typed so far. */
export interface InlineDraft {
  card: CardKey
  content: string
  initialContent: string
  initialReadme?: string
  revision: RevisionEdit
}

/** Draft of one Card's definition: `_card.json`, and its notes (README.md's
 *  description and body and _comments.json, as {description, content,
 *  comments}; see documents.ts). */
export interface DefinitionDraft {
  card: J
  meta?: J
}

/** A project inside this one (externals/<name>/, nested further): the inner
 *  canvas of a Card in Card. A copy owned by this project, edited here like
 *  the project's own documents (written back as its files). */
export interface ExternalProject {
  project: J
  layout: J
  /** Card id -> its definition files (_card.json, README.md, _comments.json). */
  definitions: Map<string, DefinitionDraft>
  /** Removed (the folder goes with it). */
}

/** The documents as files, relative to the external's folder. */
export type SyncState =
  | { kind: 'saved' }
  | { kind: 'saving' }
  | { kind: 'failed'; message: string }
  | { kind: 'offline' }
  /** Card changes wait until the run ends. */

export type EnvironmentState = EnvPublicState | { state: 'reconnecting' }

/** Undo consists solely of the host's guarded inverse edits. */
export interface UndoEntry { operation: H.ProjectEditsT }
const mapEq = <V>(a: Map<string, V>, b: Map<string, V>, eq: (x: V, y: V) => boolean) =>
  a.size === b.size && [...a].every(([k, v]) => b.has(k) && eq(v, b.get(k)!))

const ACTIVE = [D.RunState.queued, D.RunState.preparing, D.RunState.running, D.RunState.cancelling]
const ENDED = [D.CardState.succeeded, D.CardState.failed, D.CardState.cancelled, D.CardState.skipped, D.CardState.blocked]

export const errorText = (e: unknown): string => {
  if (e instanceof RPCFailure) return `${e.kindName}: ${e.diagnostic ?? ''}`
  return e instanceof Error ? e.message : String(e)
}

/** A failure to reach the tsugumid, as opposed to one while reading the project. */
class Unreached extends Error {
  constructor(readonly cause: unknown) {
    super(errorText(cause))
  }
}
type Notification = Parameters<NonNullable<RPCClient['onNotification']>>[0]

export class ProjectSession extends Observable {
  readonly presence = new SavePresence()

  projectHost = projectHost
  projectTrust: number | undefined
  trustedFolder = ''
  async refreshProjectTrust() {
    const result = await this.projectHost.trust(this.dir)
    this.projectTrust = result.decision
    this.trustedFolder = s(result.trustedFolder)
    this.changed()
  }
  async setProjectTrust(trusted: boolean) {
    try {
      const result = await this.projectHost.trust(this.dir, trusted ? H.ProjectTrustDecision.Trusted : H.ProjectTrustDecision.Untrusted)
      this.projectTrust = result.decision
      this.trustedFolder = s(result.trustedFolder)
    } catch (error) { this.connectionMessage = errorText(error) }
    this.changed()
  }

  readonly viewId: string
  readonly origin: string

  // Connection
  environmentHost = environmentHost
  private environmentKey?: string
  environment: EnvironmentState = { state: 'stopped' }
  connected = false
  connectionMessage?: string
  /** The last connection failed before the project could be read (tsugumid was not reached), not while reading it. */
  connectionFailed = false
  /** Opens the transport to a tsugumid (the tests give their own). */
  openTransport: (url: string, signal?: AbortSignal) => Promise<Transport> = webSocketTransport
  private connectionEpoch?: symbol
  private connecting?: AbortController
  private client?: RPCClient
  private subscription?: string
  private lastSeq = 0n
  projectId?: string
  endpoint?: string
  readyFile?: string
  private reconnecting = false
  private environmentChange?: Promise<unknown>

  // Confirmed document (owned by core)
  rev = 0n
  executionRev = 0n
  confirmedProject: J = obj()
  confirmedLayout: J = obj()
  confirmedDefinitions = new Map<string, J>()
  confirmedDefinitionMeta = new Map<string, J>()
  /** Externals by folder ("externals/<name>/", nested "…/externals/<name>/"). */
  externals = new Map<string, ExternalProject>()
  /** Bytes of the files the core carries: nested projects' documents and the
   *  canvas notes (notes/<id>.md). */
  private sourceDocuments = new Map<string, Uint8Array>()
  /** The confirmed layout.bin; confirmedLayout is its view with the notes. */
  private confirmedLayoutBytes: Uint8Array = new Uint8Array()
  sources = new Map<string, { hash: string; size: bigint }>()
  resolution = new Map<CardKey, ResolvedCard>()
  diagnostics: Diagnostic[] = []
  executionServices: R.ExecutionServiceT[] = []

  // Draft
  project: J = obj()
  layout: J = obj()
  /** The Draft being written on the canvas (geometry opens its Card up). */
  inlineDraft?: InlineDraft
  private draftFinishTask?: Promise<void>
  get hasUnsavedEdits() {
    const e = this.inlineDraft
    return this.dirty || !!this.draftFinishTask || !!e && e.content !== e.initialContent
  }
  /** Edited externals (by folder), written back as their files. */
  /** Base hashes chosen by an editor that read the file before this draft started. */
  undoStack: UndoEntry[] = []
  redoStack: UndoEntry[] = []
  saveError?: string
  get saving() { return this.dirty }
  /** Edits wait for the run to end (the core keeps Card changes back while it runs). */
  /** Last user-facing problem of an action (run refused, import failed, ...). */
  cacheUsage?: R.ProjectCacheUsageResultT
  notice?: string

  private readonly documentRevisions = new DocumentRevisions()
  operateProject = projectOperation
  private operationTask?: Promise<R.SaveResultT>

  // View state saved with ViewSave: the active tab's scope and selection.
  initialScope: string[] = []
  initialSelection = new Set<CardKey>()
  cameras = new Map<CardKey, { x: number; y: number; zoom: number }>()
  private viewRev = 0n
  private viewSaveTimer?: ReturnType<typeof setTimeout>
  private closed = false

  // Runs
  private stopRunId?: string
  stopError?: string
  get stopRequested() { return this.stopRunId !== undefined }
  get canStopRun() {
    return this.runActive && !this.stopRequested && this.runStatus?.state !== D.RunState.cancelling
  }
  private _runStatus?: R.RunStatusT
  get runStatus() { return this._runStatus }
  set runStatus(status: R.RunStatusT | undefined) {
    const previousId = this.currentRunId
    this._runStatus = status
    if (previousId !== this.currentRunId || !this.runActive || status?.state === D.RunState.cancelling)
      this.stopError = undefined
    if (this.stopRunId !== this.currentRunId || !this.runActive || status?.state === D.RunState.cancelling)
      this.stopRunId = undefined
  }
  cardStates = new Map<CardKey, CardRunState>()
  readonly liveViews = new LiveViews()
  lastFinished = 0
  remoteHistoryRevision = 0
  recordingRevision = 0
  batchHistoryRevision = 0
  private runBatches = new Map<CardKey, R.RunBatchT>()
  timeline: TimelineEntry[] = []
  /** Browser clock minus core clock (ms): the smallest difference seen. */
  coreClockOffset?: number
  private readonly logListeners = new Set<(notice?: R.LogAvailableT) => void>()

  subscribeLogs(listener: (notice?: R.LogAvailableT) => void) {
    this.logListeners.add(listener)
    return () => { this.logListeners.delete(listener) }
  }

  /** How many notifications the UI has taken in, and in how many batches. */
  notificationCount = 0
  notificationBatches = 0
  private offEvents: () => void
  private offHost: () => void

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
      if (e.type === 'settingsChanged') void this.refreshProjectTrust().catch(() => {})
    })
    this.offHost = onHostEvent((event) => {
      if (this.closed || this.connected) return
      if (event.valueType === H.EventPayload.EnvironmentState) {
        const state = event.value as H.EnvironmentStateT
        if (s(state.project) !== this.dir) return
        this.environmentKey = s(state.key)
        this.environment = environmentState(state)
      } else if (event.valueType === H.EventPayload.EnvironmentProgress) {
        const progress = event.value as H.EnvironmentProgressT
        if (s(progress.project) !== this.dir && s(progress.key) !== this.environmentKey) return
        this.environment = environmentProgress(progress)
      } else return
      this.changed()
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

  get dirty(): boolean { return !!this.operationTask }
  get computationDirty(): boolean { return this.dirty }
  get syncState(): SyncState {
    if (!this.connected) return { kind: 'offline' }
    if (this.saveError) return { kind: 'failed', message: this.saveError }
    return this.dirty ? { kind: 'saving' } : { kind: 'saved' }
  }
  get definitions(): Map<string, J> { return this.confirmedDefinitions }
  get definitionMeta(): Map<string, J> { return this.confirmedDefinitionMeta }
  get sourcePaths(): string[] { return [...this.sources.keys()].sort() }

  get runActive(): boolean {
    const st = this.runStatus?.state
    return st !== undefined && ACTIVE.includes(st)
  }

  get currentRunId(): string | undefined {
    return sOpt(this.runStatus?.runId)
  }

  executionBatch(card: CardKey): R.RunBatchT | undefined {
    const batch = this.runBatches.get(card)
    if (!batch || !this.currentRunId || this.runStatus?.executionRev !== this.executionRev || this.computationDirty) return
    return batch
  }

  // The state the Canvas shows is presentation: each UI derives it from run
  // notifications (execution-list batches) rather than asking hostd.
  canvasCardState(card: CardKey): D.CardState | undefined {
    const batch = this.executionBatch(card)
    if (batch?.executionList) {
      if (this.runStatus?.state === D.RunState.cancelled && (!batch.complete || batch.failed)) return D.CardState.cancelled
      return batch.complete ? (batch.failed ? D.CardState.failed : D.CardState.succeeded) : D.CardState.running
    }
    return this.cardStates.get(card)?.state
  }

  executionWire(card: CardKey, port: string, output = true): { executionList: boolean; count?: number } {
    const inner = output && this.kind(card) === 'subgraph' ? this.boundarySource(card, port) : undefined
    if (inner) return this.executionWire(...inner)
    const batch = output ? this.executionBatch(card) : undefined
    const emitted = batch?.outputs.find((item) => s(item.port) === port)
    if (emitted) return { executionList: emitted.executionList, count: emitted.executionList ? emitted.count : undefined }
    const shape = this.resolution.get(card)?.[output ? 'outputs' : 'inputs'].find((item) => item.name === port)?.executionShape
    return { executionList: shape === D.ExecutionShape.list || (batch?.executionList === true && this.kind(card) !== 'collect') }
  }

  get rpc(): RPCClient | undefined {
    return this.client
  }

  async refreshExecutionServices() {
    const client = this.client
    if (!client) return
    const snapshot = await client.call<R.ProjectSnapshotT>(R.RequestPayload.ProjectInspectRequest, new R.ProjectInspectRequestT())
    if (this.client !== client) return
    this.executionServices = snapshot.executionServices
    this.changed()
  }

  get isEditable(): boolean {
    return this.connected && !this.runActive
  }

  // ---- Connection ------------------------------------------------------------------

  start() {
    // Shown as starting at once: the loading card, never a "stopped" page.
    if (this.environment.state === 'stopped' || (this.environment.state === 'failed' && this.environment.historyVersionMismatch)) {
      this.environment = { state: 'starting', message: '' }
    }
    void this.openConnection()
  }



  private async openConnection() {
    if (this.closed) return
    const epoch = this.connectionEpoch = Symbol()
    this.connecting?.abort()
    const connection = this.connecting = new AbortController()
    this.connectionMessage = undefined
    this.changed()
    try {
      await this.refreshProjectTrust()
      connection.signal.throwIfAborted()
      const env = await this.environmentHost.start(this.dir)
      if (epoch !== this.connectionEpoch || this.closed) return
      this.environment = env
      this.changed()
      if (env.state !== 'ready') return
      const c = await this.reach(env.endpoint!, env.token, connection.signal)
      const opened = await c.open(this.viewId)
      if (this.closed || epoch !== this.connectionEpoch) {
        c.close()
        return
      }
      this.client = c
      this.projectId = env.projectId
      this.endpoint = env.endpoint
      this.readyFile = env.readyFile
      this.apply(opened.snapshot!)
      this.subscription = s(opened.subscriptionId)
      this.lastSeq = opened.seq ?? 1n
      this.connected = true
      this.listen(c)
      c.onClose = () => {
        if (this.client !== c) return
        this.documentRevisions.disconnect(new RPCError('disconnected', L('接続していません')))
        this.connected = false
        this.liveViews.reset()
        this.changed()
        this.reconnect()
      }
      if (this.makeRepositoryOnConnect) {
        this.makeRepositoryOnConnect = false
        await c.versionInit(uuid(), 'ui').catch(() => {})
      }
      this.changed()
      await Promise.all([this.refreshVisibleDisplays(), this.restoreCardStates(c)])
    } catch (e) {
      if (epoch !== this.connectionEpoch || this.closed) return
      this.connected = false
      this.liveViews.reset()
      this.connectionFailed = e instanceof Unreached
      this.connectionMessage = errorText(e instanceof Unreached ? e.cause : e)
      if (this.environment.state === 'starting' || this.environment.state === 'reconnecting') this.environment = { state: 'ready' }
      this.changed()
    } finally {
      if (this.connecting === connection) this.connecting = undefined
    }
  }

  private async reach(endpoint: string, token: string | undefined, signal: AbortSignal): Promise<RPCClient> {
    let client: RPCClient | undefined
    try {
      client = new RPCClient(await this.openTransport(endpoint, signal))
      signal.throwIfAborted()
      const provisional = client
      const cancel = () => provisional.close()
      signal.addEventListener('abort', cancel, { once: true })
      try { await client.connect(token) }
      finally { signal.removeEventListener('abort', cancel) }
      return client
    } catch (e) {
      client?.close()
      if (e instanceof RPCError && e.code === 'disconnected') throw new Unreached(e)
      throw e
    }
  }

  /** Reconnects after a lost connection: Hello -> Open -> re-read state. */
  reconnect() {
    if (this.closed || this.reconnecting || this.environmentChange) return
    this.reconnecting = true
    this.environment = { state: 'reconnecting' }
    const old = this.client
    this.client = undefined
    old?.close()
    this.connected = false
    this.liveViews.reset()
    this.changed()
    void (async () => {
      // The host Start request joins a restart and answers only when ready or
      // failed; a fixed number of browser retries cannot budget VM startup.
      if (!this.closed && !this.environmentChange) await this.openConnection()
      this.reconnecting = false
    })()
  }

  /** Pause this view while hostd changes shares and restarts the environment. */
  async withEnvironmentChange<T>(operation: () => Promise<T>): Promise<T> {
    while (this.environmentChange) { try { await this.environmentChange } catch {} }
    if (this.closed) throw new Error(L('接続していません'))
    const run = (async () => {
      let completed: { value: T } | undefined
      this.connectionEpoch = Symbol()
      const old = this.client
      this.client = undefined
      old?.close()
      this.connected = false
      this.liveViews.reset()
      this.environment = { state: 'starting', message: L('tsugumid の応答を待っています') }
      this.changed()
      try {
        const value = await operation()
        completed = { value }
        await this.openConnection()
        if (!this.connected) throw new Error(this.connectionMessage ?? L('接続していません'))
        return value
      } catch (error) {
        if (!this.connected) await this.openConnection()
        if (completed && this.connected) return completed.value
        throw error
      }
    })()
    this.environmentChange = run
    try { return await run } finally { if (this.environmentChange === run) this.environmentChange = undefined }
  }

  /** The project was left: no more connections, whatever is under way. */
  close() {
    this.closed = true
    this.connecting?.abort()
    this.offEvents?.()
    this.offHost?.()
    clearTimeout(this.viewSaveTimer)
    const c = this.client
    this.client = undefined
    c?.close()
    this.connected = false
    this.liveViews.reset()
  }

  async stopEnvironment() {
    this.connectionEpoch = Symbol()
    this.connecting?.abort()
    const c = this.client
    this.client = undefined
    c?.close()
    this.connected = false
    this.liveViews.reset()
    this.changed()
    try { this.environment = await this.environmentHost.stop(this.dir) }
    catch (error) { this.connectionMessage = errorText(error) }
    this.changed()
  }

  dispose() {
    this.offEvents()
    this.offHost()
    this.client?.close()
    this.liveViews.reset()
  }

  /** Notifications are taken in as they arrive and applied in batches at most
   *  once per frame (or four times a second with no frames): a run with thousands of short invocations causes one
   *  view update per batch, not per event. */
  private listen(c: RPCClient) {
    let inbox: Notification[] = []
    c.onNotification = (e) => {
      if (this.client !== c) return
      inbox.push(e)
      if (inbox.length > 1) return
      const apply = () => {
        cancelAnimationFrame(frame)
        clearTimeout(timer)
        const batch = inbox
        inbox = []
        if (this.client !== c) return
        this.notificationBatches++
        for (const ev of batch) {
          if (this.client !== c) break
          this.handle(ev)
        }
        this.changed()
      }
      const frame = requestAnimationFrame(apply)
      // A tab in the background has no frames: the run still goes on, and must be seen to.
      const timer = setTimeout(apply, 250)
    }
  }

  /** What a Card's invocation says about the Card, from a notification or from the run's record. */
  private noteInvocation(inv: R.InvocationSummaryT) {
    const k = key(segs(inv.card))
    const prev = this.cardStates.get(k)
    const n = inv.invocation ?? 0n
    if (!prev || prev.invocation <= n) {
      const sent = new Set(inv.outputsSent.map((x) => s(x)))
      this.cardStates.set(k, {
        state: inv.state, invocation: n, phase: inv.phase, failure: sOpt(inv.failure?.diagnostic),
        executionState: inv.executionState,
        transfer: (inv.state === D.CardState.running || inv.state === D.CardState.preparing) && inv.transferDirection && inv.transferDone < inv.transferTotal
          ? L('ファイル転送: {0} ({1}/{2} bytes)', s(inv.transferName), String(inv.transferDone), String(inv.transferTotal)) : undefined,
        // Each notification carries the invocation's stages so far.
        stages: stageRecords(inv.stages), cached: !!inv.buildCached, workerReused: !!inv.workerReused,
        sent, sentInRun: new Set([...(prev?.sentInRun ?? []), ...sent]),
        startedAt: inv.startedAt !== null ? Number(inv.startedAt) : undefined, endedAt: inv.endedAt !== null ? Number(inv.endedAt) : undefined,
        readyAt: inv.readyAt !== null ? Number(inv.readyAt) : undefined,
      })
    }
  }

  private noteBatch(batch: R.RunBatchT) {
    const card = key(segs(batch.card))
    const current = this.runBatches.get(card)
    if (current && (batch.activation < current.activation ||
      (batch.activation === current.activation && current.complete && !batch.complete))) return
    this.runBatches.set(card, batch)
  }

  /** The Cards of the last run, for a project just opened: their notifications were sent before it was. */
  private async restoreCardStates(c: RPCClient) {
    const run = sOpt(this.runStatus?.runId)
    if (!run) return
    const page = await c.latestInvocations(run).catch(() => undefined)
    // A run started meanwhile has its own states.
    if (!page || sOpt(this.runStatus?.runId) !== run) return
    for (const inv of page.records) this.noteInvocation(inv)
    for (const batch of page.batches) this.noteBatch(batch)
    this.changed()
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
      case R.NotificationPayload.CardViewsUpdated: {
        const { card, views, failure } = e.value as R.CardViewsUpdatedT
        this.liveViews.get(key(segs(card)))?.declare(views.map(v => ({ name: s(v.name), type: s(v.type), blob: s(v.blob) })),
          failure ? s(failure.diagnostic) || (D.FailureKind[failure.kind] ?? String(failure.kind)) : undefined)
        break
      }
      case R.NotificationPayload.DisplayUpdated: {
        const { card, view, bytes, creditId } = e.value as LiveDisplayUpdate
        const client = this.client
        const complete = creditId === undefined ? undefined : () => {
          void client?.viewCredit(e.subscriptionId, card, view, bytes.length, creditId).catch(() => {})
        }
        const views = this.liveViews.get(key(card))
        if (views) views.data(view, bytes, complete)
        else complete?.()
        break
      }
      case R.NotificationPayload.RunEvent:
        this.applyRunEvent(e.value as R.RunEventT)
        break
      case R.NotificationPayload.RunCategoryDeleteResult:
        this.recordingRevision++
        this.changed()
        break
      case R.NotificationPayload.LogAvailable:
        for (const listener of this.logListeners) listener(e.value as R.LogAvailableT)
        break
    }
  }

  // ---- Applying core state -----------------------------------------------------------

  private readDefinitions(defs: R.DefinitionT[]) {
    for (const d of defs) {
      const id = s(d.id)
      const c = tryParse(s(d.cardJson))
      if (c !== undefined) this.confirmedDefinitions.set(id, c)
      const m = cardNotes(sOpt(d.readme), sOpt(d.commentsJson))
      if (m !== undefined) this.confirmedDefinitionMeta.set(id, m)
      else this.confirmedDefinitionMeta.delete(id)
    }
  }

  private apply(snap: R.ProjectSnapshotT) {
    this.executionServices = snap.executionServices
    this.rev = snap.rev ?? 0n
    this.documentRevisions.reset(this.rev)
    this.executionRev = snap.executionRev ?? 0n
    this.confirmedProject = tryParse(s(snap.projectJson)) ?? obj()
    this.confirmedLayoutBytes = Uint8Array.from(snap.layout)
    this.confirmedDefinitions = new Map()
    this.confirmedDefinitionMeta = new Map()
    this.readDefinitions(snap.definitions)
    this.sources = new Map()
    this.sourceDocuments.clear()
    this.externals.clear()
    this.derivedLayouts.clear()
    this.layoutTasks.clear()
    this.applySources(snap.sources, [])
    this.confirmedLayout = this.layoutAt('')
    this.refreshDerivedLayout()
    this.applyResolution(snap.resolution, snap.diagnostics)
    if (this.currentRunId !== sOpt(snap.run?.runId)) this.runBatches = new Map()
    this.runStatus = snap.run ?? undefined
    this.viewRev = snap.viewRev ?? 0n
    const view = readViewState(snap.view)
    if (view) {
      this.initialScope = view.scope
      for (const cam of view.cameras) this.cameras.set(key(cam.scope), { x: cam.x, y: cam.y, zoom: cam.zoom })
      this.initialSelection = new Set(view.selected_cards.map(key))
    }
    this.project = clone(this.confirmedProject)
    this.layout = clone(this.confirmedLayout)
    this.documentRevisions.drain(delta => this.applyOrderedDelta(delta))
  }

  private applyResolution(res: R.ResolvedCardT[], diags: D.DiagnosticT[]) {
    const r = new Map<CardKey, ResolvedCard>()
    const all = diags.map(diag)
    const port = (x: D.PortT): PortInfo => ({
      name: s(x.name), rootType: sOpt(x.rootType), executionShape: x.executionShape,
      mergeMemberTypes: x.mergeMemberTypes.map(s),
    })
    for (const rc of res) {
      const p = segs(rc.card)
      const d = rc.diagnostics.map(diag)
      r.set(key(p), { path: p, inputs: rc.inputs.map(port), outputs: rc.outputs.map(port), diagnostics: d,
        nixReferences: rc.nixReferences.map(r => ({ target: segs(r.target), name: s(r.name) })),
        parameters: (rc.parameters ?? []).flatMap(p => {
          const value = tryParse(s(p.valueJson))
          if (value === undefined) return []
          return [{ name: s(p.name), type: s(p.type), value }]
        }) })
      all.push(...d)
    }
    for (const [card, views] of this.liveViews) {
      if (this.resolution.has(card) && !r.has(card)) views.clear()
    }
    this.resolution = r
    this.diagnostics = all
  }

  private applyDelta(d: R.DocumentDeltaT) {
    this.documentRevisions.receive(d, delta => this.applyOrderedDelta(delta))
  }

  private applyOrderedDelta(d: R.DocumentDeltaT) {
    if (d.baseRev !== this.rev) {
      if (d.rev !== null && d.rev <= this.rev) return // already applied (our own save reply)
      this.reconnect()
      return
    }
    const beforePresence = connectionsOf(this.confirmedProject)
    this.presence.finish()
    this.rev = d.rev ?? this.rev
    this.executionRev = d.executionRev ?? this.executionRev
    if (d.projectJson !== null) {
      const v = tryParse(s(d.projectJson))
      if (v !== undefined) this.confirmedProject = v
    }
    if (d.layoutChanged) this.confirmedLayoutBytes = Uint8Array.from(d.layout)
    for (const id of d.definitionsRemoved) {
      this.confirmedDefinitions.delete(s(id))
      this.confirmedDefinitionMeta.delete(s(id))
    }
    this.readDefinitions(d.definitionsUpsert)
    this.applySources(d.sourcesUpsert, d.sourcesRemoved.map(s))
    this.confirmedLayout = this.layoutAt('')
    if (d.origin === 'agent') {
      const cards = [...d.definitionsUpsert.map(x => s(x.id)), ...d.sourcesUpsert.flatMap(x => {
        const parts = s(x.path).split('/')
        return parts[0] === 'definitions' && parts.length >= 3 ? [parts[1]] : []
      })]
      this.presence.saved(cards, beforePresence, connectionsOf(this.confirmedProject), Date.now() / 1000)
    }
    this.refreshDerivedLayout()
    this.applyResolution(d.resolution, d.diagnostics)
    this.project = clone(this.confirmedProject)
    this.layout = clone(this.confirmedLayout)
  }

  private applyRunEvent(e: R.RunEventT) {
    switch (e.valueType) {
      case R.RunEventValue.RunAcceptedEvent: {
        this.presence.finish()
        const st = (e.value as R.RunAcceptedEventT).status
        if (st && this.currentRunId !== s(st.runId)) {
          this.runBatches = new Map()
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
        break
      }
      case R.RunEventValue.RunCardChanged: {
        const inv = (e.value as R.RunCardChangedT).invocation
        if (!inv) break
        if (inv.batchActivation != null) {
          this.batchHistoryRevision++
          if (s(e.runId) !== this.currentRunId) break
        }
        if (inv.executionState !== D.RemoteExecutionState.unspecified) {
          this.remoteHistoryRevision++
          for (const listener of this.logListeners) listener()
          // Recovery also publishes events for runs already in history.
          if (s(e.runId) !== this.currentRunId) break
        }
        this.noteInvocation(inv)
        const lastStart = inv.stages.length ? inv.stages[inv.stages.length - 1].start : null
        this.sampleClock(inv.endedAt ?? (inv.state === D.CardState.running ? lastStart : null))
        this.recordTimeline(key(segs(inv.card)), inv)
        break
      }
      case R.RunEventValue.RunBatchChanged: {
        const batch = (e.value as R.RunBatchChangedT).batch
        if (batch) {
          this.batchHistoryRevision++
          if (s(e.runId) === this.currentRunId) this.noteBatch(batch)
        }
        break
      }
      case R.RunEventValue.RunFinished: {
        const st = (e.value as R.RunFinishedT).summary?.status
        if (st) this.runStatus = st
        this.lastFinished = Date.now()
        break
      }
    }
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

  /** Confirmed definitions as definition files (card, meta, link). */
  get confirmedDefinitionDrafts(): Map<string, DefinitionDraft> {
    const d = new Map<string, DefinitionDraft>()
    for (const [id, c] of this.confirmedDefinitions) d.set(id, { card: c, meta: this.confirmedDefinitionMeta.get(id) })
    return d
  }

  get definitionDrafts(): Map<string, DefinitionDraft> { return this.confirmedDefinitionDrafts }
  get effectiveExternals(): Map<string, ExternalProject> { return this.externals }

  /** Changes an external's documents in the draft. */
  updateExternal(prefix: string, fn: (e: ExternalProject) => void) {
    const before = this.externals.get(prefix)
    if (!prefix || !before) return
    const after = cloneExternal(before); fn(after)
    this.editJSON(prefix + 'tsugumi.json', before.project, after.project)
    this.editJSON(prefix + 'layout.bin', before.layout, after.layout)
    for (const [id, d] of after.definitions) {
      const old = before.definitions.get(id), dir = prefix + 'definitions/' + id + '/'
      this.editJSON(dir + '_card.json', old?.card, d.card)
      this.editNotes(dir, old?.meta, d.meta)
    }
  }

  /** Applies an edit and saves it at once. One call is one step of undo
   *  (returned; nothing when the edit changed nothing). */
  edit(change: (s: this) => void): void {
    void this.applyMutation(change).catch(e => this.cardEditFailed(e))
  }

  private commandEdits?: H.ProjectEditT[]

  jsonEdit(file: string, before: J | undefined, after: J | undefined): H.ProjectEditT {
    return new H.ProjectEditT(H.ProjectEditValue.ProjectJSONEdit, new H.ProjectJSONEditT(file,
      new H.ProjectValueT(before !== undefined, before === undefined ? null : serialize(before)),
      new H.ProjectValueT(after !== undefined, after === undefined ? null : serialize(after))))
  }

  /** Edits of a Card's notes: its README.md (as a JSON string) and _comments.json. */
  notesEdits(dir: string, before: J | undefined, after: J | undefined): H.ProjectEditT[] {
    const was = splitCardNotes(before), now = splitCardNotes(after)
    const edits: H.ProjectEditT[] = []
    if (was.readme !== now.readme) edits.push(this.jsonEdit(dir + 'README.md', was.readme, now.readme))
    if (!jeq(was.comments, now.comments)) edits.push(this.jsonEdit(dir + '_comments.json', was.comments, now.comments))
    return edits
  }

  private editNotes(dir: string, before: J | undefined, after: J | undefined) {
    const edits = this.notesEdits(dir, before, after)
    if (!edits.length) return
    if (this.commandEdits) this.commandEdits.push(...edits)
    else void this.operateFields(edits).catch(e => this.cardEditFailed(e))
  }

  private editJSON(file: string, before: J | undefined, after: J | undefined) {
    if (jeq(before, after)) return
    const edit = this.jsonEdit(file, before, after)
    if (this.commandEdits) this.commandEdits.push(edit)
    else void this.operateFields([edit]).catch(e => this.cardEditFailed(e))
  }

  appendEdits(edits: H.ProjectEditT[]) {
    if (!this.commandEdits) throw new Error('Missing operation builder')
    this.commandEdits.push(...edits)
  }

  async applyMutation(change: (s: this) => void): Promise<bigint> {
    const result = await this.operate(H.ProjectOperationBody.ProjectEdits, () => {
      this.commandEdits = []
      try { change(this); return new H.ProjectEditsT(this.commandEdits) }
      finally { this.commandEdits = undefined }
    })
    return result.rev ?? this.rev
  }

  setProjectField(name: string, value: J) {
    const before = clone(this.project), after = clone(before)
    if (!isObj(after)) throw new Error('Invalid project')
    after.set(name, value)
    this.editJSON('tsugumi.json', before, after)
  }

  async replaceDocuments(expectedRev: bigint, documents: [string, J | undefined][]) {
    const result = await this.operate(H.ProjectOperationBody.ProjectReplaceDocuments,
      new H.ProjectReplaceDocumentsT(expectedRev, documents.map(([file, value]) => new H.ProjectDocumentT(file, value !== undefined, value === undefined ? null : serialize(value)))))
    return result.rev ?? this.rev
  }

  async undo() { await this.revert(true) }
  async redo() { await this.revert(false) }

  private async revert(undo: boolean) {
    try {
      await this.operate(H.ProjectOperationBody.ProjectEdits, () =>
        (undo ? this.undoStack : this.redoStack).at(-1)?.operation ?? new H.ProjectEditsT(), false, result => {
          const inverse = operationInverse(result)
          ;(undo ? this.undoStack : this.redoStack).pop()
          if (inverse.edits.length) (undo ? this.redoStack : this.undoStack).push({ operation: inverse })
        })
    } catch (error) { this.saveError = errorText(error); this.changed() }
  }

  discardDraft() { this.inlineDraft = undefined; this.changed() }

  async settleBeforeLeaving(giveUp: (reason: string) => Promise<boolean>): Promise<boolean> {
    await this.finishDraftEdit()
    await this.save()
    if (!this.hasUnsavedEdits) return true
    if (!await giveUp(this.saveError ?? this.connectionMessage ?? L('変更を保存できませんでした。'))) return false
    this.discardDraft()
    return true
  }

  /** Submit one action, then complete only after its reply is in the confirmed state. */
  async operate(bodyType: H.ProjectOperationBody, body: H.ProjectOperateRequestT['body'] | (() => H.ProjectOperateRequestT['body']), record = true, applied?: (result: R.SaveResultT) => void): Promise<R.SaveResultT> {
    const previous = this.operationTask
    const task = (async () => {
      await previous?.catch(() => undefined)
      const client = this.client
      if (!this.connected || !client || !this.projectId) throw new RPCError('disconnected', L('接続していません'))
      const payload = typeof body === 'function' ? body() : body
      if (bodyType === H.ProjectOperationBody.ProjectEdits && !(payload as H.ProjectEditsT).edits.length)
        return Object.assign(new R.SaveResultT(), { rev: this.rev })
      const request = new H.ProjectOperateRequestT(this.projectId, uuid(), bodyType, payload)
      const result = await this.operateProject(request)
      if (!this.connected || this.client !== client) throw new RPCError('disconnected', L('接続していません'))
      if (result.delta) this.applyDelta(result.delta)
      await this.documentRevisions.applied(result.rev ?? this.rev)
      const inverse = record ? operationInverse(result) : new H.ProjectEditsT()
      if (record && inverse.edits.length) {
        this.undoStack.push({ operation: inverse })
        if (this.undoStack.length > 500) this.undoStack.shift()
        this.redoStack = []
      }
      applied?.(result)
      this.saveError = undefined
      this.changed()
      return result
    })()
    this.operationTask = task
    try { return await task }
    finally { if (this.operationTask === task) this.operationTask = undefined }
  }

  /** Field values are captured at the gesture, never diffed against an editor save base. */
  fieldEdit(file: string, selector: (string | JObj)[], oldValue: J | undefined, newValue: J | undefined): H.ProjectEditT {
    return new H.ProjectEditT(H.ProjectEditValue.ProjectFieldEdit,
      new H.ProjectFieldEditT(file, selector.map(step => typeof step === 'string'
        ? new H.ProjectSelectorT(step) : new H.ProjectSelectorT(null, serialize(step))),
      new H.ProjectValueT(oldValue !== undefined, oldValue === undefined ? null : serialize(oldValue)),
      new H.ProjectValueT(newValue !== undefined, newValue === undefined ? null : serialize(newValue))))
  }

  async operateFields(edits: H.ProjectEditT[] | (() => H.ProjectEditT[])) {
    if (typeof edits === 'function') await this.operate(H.ProjectOperationBody.ProjectEdits, () => new H.ProjectEditsT(edits()))
    else if (edits.length) await this.operate(H.ProjectOperationBody.ProjectEdits, new H.ProjectEditsT(edits))
  }

  /** Wait for submitted actions; editors retain any rejected text themselves. */
  async save() { await this.operationTask?.catch(e => { this.saveError = errorText(e) }) }

  reloadApproval?: { baseRev: bigint; permissions: string[] }
  reloadingExternal = false

  async answerReloadApproval(allow: boolean, selected: string[]) {
    const request = this.reloadApproval
    this.reloadApproval = undefined
    this.changed()
    if (allow && request) await this.reloadExternal(request.permissions.filter(p => selected.includes(p)), request.baseRev)
  }

  async reloadExternal(approved: string[] = [], baseRev = this.rev) {
    if (!this.client || this.reloadingExternal) return
    this.reloadingExternal = true
    this.changed()
    try {
      await this.operate(H.ProjectOperationBody.ProjectReload, new H.ProjectReloadT(baseRev, approved), false)
      this.saveError = undefined
    } catch (e) {
      if (e instanceof RPCFailure && e.kind === D.FailureKind.approval_required && e.permissions.length) {
        this.reloadApproval = { baseRev, permissions: [...new Set([...approved, ...e.permissions])] }
        this.saveError = undefined
      } else this.saveError = errorText(e)
    } finally { this.reloadingExternal = false }
    this.changed()
  }

  // ---- View state ------------------------------------------------------------------

  saveViewSoon(scope: string[], selection: Set<CardKey>) {
    clearTimeout(this.viewSaveTimer)
    this.viewSaveTimer = setTimeout(async () => {
      const client = this.client
      if (!client) return
      const v = writeViewState({ scope,
        cameras: [...this.cameras].map(([k, c]) => ({ scope: path(k), ...c })),
        selected_cards: [...selection].sort().map(path),
      })
      try {
        const r = await client.saveView(this.viewId, this.viewRev, v)
        this.viewRev = r.viewRev ?? this.viewRev
      } catch (e) {
        if (e instanceof RPCFailure && e.kind === D.FailureKind.revision_conflict) this.viewRev = e.currentRev ?? this.viewRev
      }
    }, 600)
  }

  /** Subscribe only while at least one live viewer for a Card is open. */
  observeDisplay(card: CardKey): () => void {
    return this.liveViews.observe(card, () => { void this.refreshVisibleDisplays() })
  }

  private async refreshVisibleDisplays() {
    if (!this.client || !this.subscription) return
    const cards = [...this.liveViews.keys()].sort().map(path)
    await this.client.setVisible(this.subscription, cards, cards.length > 0).catch(() => {})
  }

  // ---- Runs --------------------------------------------------------------------

  async run(card: string[], propagate: boolean) {
    await this.finishDraftEdit()
    if (this.inlineDraft) throw new RPCFailure({ kind: D.FailureKind.validation_failed, diagnostic: L('変更を保存できないため実行できません。') })
    const client = this.client
    if (!client) throw new RPCError('disconnected', L('接続していません'))
    if (this.computationDirty) await this.save()
    if (this.computationDirty) throw new RPCFailure({ kind: D.FailureKind.validation_failed, diagnostic: this.saveError ?? L('変更を保存できないため実行できません。') })
    const parallel = Math.max(1, Math.min(64, prefs.maxConcurrency))
    try {
      const accepted = await client.run(this.rev, uuid(), card, propagate, parallel)
      // A fast or empty run may already have reported newer state.
      if (this.currentRunId !== sOpt(accepted.run?.runId)) {
        this.runBatches = new Map()
        this.runStatus = accepted.run ?? undefined
        this.cardStates = new Map()
        this.timeline = []
      }
      this.changed()
    } catch (e) {
      if (e instanceof RPCError && e.code === 'disconnected') this.reconnect() // never re-send blindly
      throw e
    }
  }

  async cancelRun() {
    const id = this.currentRunId
    if (!id || !this.canStopRun) return
    this.stopRunId = id
    this.stopError = undefined
    this.changed()
    try {
      if (!this.client) throw new RPCError('disconnected', L('接続していません'))
      const status = await this.client.cancel(id)
      if (this.stopRunId !== id || sOpt(status.runId) !== id) return
      this.runStatus = status
    } catch (error) {
      if (this.stopRunId !== id) return
      this.stopRunId = undefined
      this.stopError = L('停止を要求できませんでした（{0}）', errorText(error))
    }
    this.changed()
  }

  // ---- Document helpers ------------------------------------------------------------

  private extLink(prefix: string, id: string): string | undefined {
    const card = prefix === '' ? this.definitions.get(id) : this.external(prefix)?.definitions.get(id)?.card
    return str(get(card, 'external'))
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
    const before = prefix ? this.externals.get(prefix)?.project : this.project
    if (!before) return
    const after = clone(before), graph = clone(get(before, 'canvas')) ?? obj()
    if (!isObj(after) || !isObj(graph)) return
    fn(graph); after.set('canvas', graph)
    this.editJSON(prefix + 'tsugumi.json', before, after)
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
    const before = this.layoutOf(scope), after = clone(before)
    if (!isObj(after)) return
    fn(after); this.editJSON(prefix + 'layout.bin', before, after)
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
    return d && definitionKind(d.card) !== 'subgraph' ? d.card : undefined
  }

  /** The Card's notes: {description, content, comments}. */
  cardMeta(k: CardKey): J | undefined {
    return this.cardDefinitionDraft(k)?.meta
  }

  /** Sets a Card's definition files in the draft (undefined removes them). */
  setCardDefinition(k: CardKey, d: DefinitionDraft | undefined) {
    const dir = this.definitionDir(k), before = this.cardDefinitionDraft(k)
    if (!dir || !d) return
    this.editJSON(dir + '_card.json', before?.card, d.card)
    this.editNotes(dir, before?.meta, d.meta)
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
    const boundary = ext ? get(this.external(ext)?.project, 'boundary') : undefined
    if (!boundary) return undefined
    return obj(['inputs', 'outputs'].map(side => [side, obj((arr(get(boundary, side)) ?? []).flatMap(port => {
      const name = str(get(port, 'name'))
      return name ? [[name, port]] : []
    }))]))
  }

  /** Changes a Card's _card.json in the draft. */
  updateCard(k: CardKey, fn: (c: JObj) => void) {
    const d = this.cardDefinitionDraft(k)
    if (!d || definitionKind(d.card) === 'subgraph') return
    const n = cloneDef(d)
    if (!isObj(n.card)) n.card = obj()
    fn(n.card as JObj)
    this.setCardDefinition(k, n)
  }

  /** Changes a Card's notes in the draft. */
  updateMeta(k: CardKey, fn: (m: JObj) => void) {
    const d = this.cardDefinitionDraft(k)
    if (!d) return
    const n = cloneDef(d)
    if (!isObj(n.meta)) n.meta = obj()
    fn(n.meta as JObj)
    this.setCardDefinition(k, n)
  }

  // ---- Comments on a Card (its _comments.json) ------------------------------------------

  /** What the user asks of this Card, in the order written. */
  comments(k: CardKey): CardComment[] {
    return cardComments(this.cardMeta(k))
  }

  /** Comments not resolved yet. */
  openComments(k: CardKey): CardComment[] {
    return this.comments(k).filter((c) => !c.resolved)
  }

  changeRequests(k: CardKey): CardComment[] { return this.openComments(k).filter(c => !c.finding) }

  /** The Cards of a scope with change requests not resolved yet. */
  cardsWithOpenComments(scope: string[]): CardKey[] {
    return this.cards(scope).map((c) => str(get(c, 'id'))).filter((x): x is string => !!x)
      .map((id) => key([...scope, id])).filter((k) => this.changeRequests(k).length > 0)
  }

  /** Changes comments through the common definition setter. */
  private changeComments(k: CardKey, fn: (list: J[]) => J[]) {
    const d = this.cardDefinitionDraft(k)
    if (!d) return
    const n = cloneDef(d)
    const meta: JObj = isObj(n.meta) ? n.meta : obj()
    const list = fn((arr(meta.get('comments')) ?? []).map((c) => clone(c)))
    meta.set('comments', list.length ? list : undefined)
    n.meta = meta.size ? meta : undefined
    this.setCardDefinition(k, n)
  }

  /** Immediate comment actions have no open working copy. */
  private editComments(k: CardKey, fn: (list: J[]) => J[]) {
    this.edit((x) => x.changeComments(k, fn))
  }

  async addComment(k: CardKey, text: string, finding = false) {
    const t = text.trim()
    if (!t) return
    const id = await this.reserveReference('comment', k)
    await this.applyMutation(x => x.changeComments(k, list => [...list, obj([['id', id], ['text', t], ['author', 'user'], ['finding', finding]])]))
  }

  /** Marks a comment by hand. Opening one again takes back what was said about it. */
  setCommentResolved(k: CardKey, id: string, resolved: boolean) {
    this.editComments(k, (list) => list.map((c) => {
      if (str(get(c, 'id')) === id && isObj(c)) {
        c.set('resolved', resolved ? true : undefined)
        c.set('state_by', 'user')
        if (!resolved) {
          c.set('resolution', undefined)
          c.set('cards', undefined)
        }
      }
      return c
    }))
  }

  removeComment(k: CardKey, id: string) {
    this.editComments(k, (list) => list.filter((c) => str(get(c, 'id')) !== id))
  }

  derivedLayouts = new Map<string, J>()
  private layoutTasks = new Map<string, Promise<void>>()

  refreshDerivedLayout(prefix = '') {
    const graph = prefix ? this.externals.get(prefix)?.project ?? obj() : this.confirmedProject
    const saved = prefix ? this.externals.get(prefix)?.layout ?? obj() : this.confirmedLayout
    const definitions = prefix ? this.externals.get(prefix)?.definitions ?? new Map<string, DefinitionDraft>() : this.confirmedDefinitionDrafts
    const task = this.layoutHost.complete(serialize(graph), serialize(saved),
      Object.fromEntries([...definitions].map(([id, d]) => [id, serialize(d.card)])))
      .then(text => {
        const currentGraph = prefix ? this.externals.get(prefix)?.project ?? obj() : this.confirmedProject
        const currentLayout = prefix ? this.externals.get(prefix)?.layout ?? obj() : this.confirmedLayout
        const currentDefinitions = prefix ? this.externals.get(prefix)?.definitions ?? new Map<string, DefinitionDraft>() : this.confirmedDefinitionDrafts
        if (this.layoutTasks.get(prefix) !== task || !jeq(graph, currentGraph) || !jeq(saved, currentLayout) || !mapEq(definitions, currentDefinitions, defEq)) return
        this.derivedLayouts.set(prefix, parse(text))
        this.changed()
      }).catch(error => {
        if (this.layoutTasks.get(prefix) !== task) return
        this.derivedLayouts.delete(prefix)
        this.notice = errorText(error)
        this.changed()
      })
    this.layoutTasks.set(prefix, task)
    return task
  }

  /** Waits for the placement inputs currently shown in this scope. */
  async waitForPlacement(k: CardKey): Promise<boolean> {
    const scope = path(k).slice(0, -1)
    while (true) {
      const prefix = this.projectPrefix(scope)
      if (prefix === undefined || !this.instance(k)) return false
      const saved = get(this.layoutOf(scope), 'cards', this.layoutKey(k))
      if (dbl(get(saved, 'x')) !== undefined && dbl(get(saved, 'y')) !== undefined) return true
      const layout = this.layoutTasks.get(prefix)
      await layout
      if (layout !== this.layoutTasks.get(prefix)) continue
      return this.projectPrefix(scope) === prefix && this.instance(k) !== undefined &&
        get(this.derivedLayouts.get(prefix), 'cards', this.layoutKey(k)) !== undefined
    }
  }

  position(k: CardKey): { x: number; y: number } {
    const scope = path(k).slice(0, -1)
    const key = this.layoutKey(k)
    const saved = get(this.layoutOf(scope), 'cards', key)
    const x = dbl(get(saved, 'x')), y = dbl(get(saved, 'y'))
    if (x !== undefined && y !== undefined) return { x, y }
    const derived = get(this.derivedLayouts.get(this.projectPrefix(scope) ?? ''), 'cards', key)
    return { x: dbl(get(derived, 'x')) ?? 0, y: dbl(get(derived, 'y')) ?? 0 }
  }

  /** The text of a carried file. */
  private carriedText(path: string): string | undefined {
    const bytes = this.sourceDocuments.get(path)
    return bytes ? new TextDecoder().decode(bytes) : undefined
  }

  /** The layout view of a project ("" or "externals/<p>/…"): its layout.bin
   *  and the text of its notes. */
  private layoutAt(prefix: string): JObj {
    const notes = new Map<string, string>()
    for (const path of this.sourceDocuments.keys()) {
      const id = noteId(prefix, path)
      if (id !== undefined && path.slice(prefix.length).split('/').length === 2) notes.set(id, this.carriedText(path) ?? '')
    }
    try {
      return layoutView(prefix ? this.sourceDocuments.get(prefix + 'layout.bin') : this.confirmedLayoutBytes, notes)
    } catch (error) {
      this.notice = `${prefix}layout.bin: ${errorText(error)}`
      return obj()
    }
  }

  private applySources(upserts: R.SourceInfoT[], removed: string[]) {
    let changed = false
    for (const path of removed) {
      this.sources.delete(path)
      changed = this.sourceDocuments.delete(path) || changed
    }
    for (const src of upserts) {
      const path = s(src.path)
      this.sources.set(path, { hash: s(src.hash), size: src.sizeBytes ?? 0n })
      // A carried file: a nested project's documents or a note (which may be empty).
      if (!src.document.length && noteId(path.slice(0, path.lastIndexOf('notes/')), path) === undefined) continue
      changed = true
      this.sourceDocuments.set(path, Uint8Array.from(src.document))
    }
    if (!changed) return
    const json = (path: string): J | undefined => {
      const text = this.carriedText(path)
      if (text === undefined) return undefined
      try { return parse(text) } catch (error) {
        this.notice = `${path}: ${errorText(error)}`
        return undefined
      }
    }
    const found = new Map<string, ExternalProject>()
    for (const path of this.sourceDocuments.keys()) {
      if (!path.endsWith('/tsugumi.json')) continue
      const prefix = path.slice(0, -'tsugumi.json'.length)
      const project = json(path)
      if (project === undefined) continue
      found.set(prefix, { project, layout: this.layoutAt(prefix), definitions: new Map() })
    }
    for (const path of this.sourceDocuments.keys()) {
      if (!path.endsWith('/_card.json')) continue
      const card = json(path)
      if (card === undefined) continue
      const dir = path.slice(0, -'_card.json'.length)
      const split = dir.lastIndexOf('/', dir.length - 2) + 1
      const id = dir.slice(split, -1)
      const prefix = dir.slice(0, split - 'definitions/'.length)
      found.get(prefix)?.definitions.set(id, {
        card, meta: cardNotes(this.carriedText(dir + 'README.md'), this.carriedText(dir + '_comments.json')),
      })
    }
    const previous = this.externals
    this.externals = found
    for (const prefix of previous.keys()) if (!found.has(prefix)) {
      this.derivedLayouts.delete(prefix)
      this.layoutTasks.delete(prefix)
    }
    for (const [prefix, next] of found) {
      const old = previous.get(prefix)
      if (!old || !jeq(old.project, next.project) || !jeq(old.layout, next.layout) ||
        !mapEq(old.definitions, next.definitions, defEq)) void this.refreshDerivedLayout(prefix)
    }
  }

  // ---- Reading -------------------------------------------------------------------

  kind(k: CardKey): CardKind {
    return definitionKind(this.cardDefinitionDraft(k)?.card)
  }

  /** A Card's name: its id, the only name it has. */
  displayName(k: CardKey): string {
    return path(k).pop() || k
  }

  /** A human-readable path. Pass the original segments to tools and links. */
  displayPath(segments: string[]): string {
    return segments.map((_, i) => this.displayName(key(segments.slice(0, i + 1)))).join('/')
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

  /** Definition folders of this project that no Card uses. */
  get unplacedDefinitions(): string[] {
    const used = new Set(this.cards([]).map((c) => str(get(c, 'id'))))
    return [...this.definitions.keys()].filter((id) => !used.has(id)).sort()
  }

  /** Places a definition folder no Card uses as its Card (the same name). */
  placeDefinition(id: string, at: { x: number; y: number }): CardKey {
    this.selectOperation(H.ProjectSelectionAction.Place, [], [key([id])], [], undefined, at)
    return key([id])
  }

  private selectOperation(action: H.ProjectSelectionAction, scope: string[], cards: Iterable<CardKey>, notes: Iterable<string> = [], connections?: J[], at = { x: 0, y: 0 }) {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) return
    void this.operate(H.ProjectOperationBody.ProjectSelection, new H.ProjectSelectionT(prefix.replace(/\/$/, ''), action,
      [...cards].map(k => this.layoutKey(k)).sort(), [...notes].sort(),
      connections ? serialize(connections) : null, at.x, at.y)).catch(e => this.cardEditFailed(e))
  }

  deleteSelection(cards: Set<CardKey>, notes: Set<string>, scope: string[]) {
    this.selectOperation(H.ProjectSelectionAction.Delete, scope, cards, notes)
  }

  /** Source text as the draft sees it. */
  async sourceText(p: string): Promise<string> {
    if (!this.client) throw new RPCError('disconnected', L('接続していません'))
    return new TextDecoder().decode(await this.client.readFile(p, this.rev))
  }

  cardEditFailed(e: unknown) {
    this.notice = errorText(e)
    this.changed()
  }

  collapsed(k: CardKey): boolean {
    return bool(get(this.layoutOf(path(k).slice(0, -1)), 'cards', this.layoutKey(k), 'collapsed')) ?? false
  }

  // ---- Cards and connections ---------------------------------------------------------

  /** A replaceable RPC boundary for clients that embed the session. */
  cardIdProvider?: (name: string, projectPath: string) => Promise<string>

  /** All new identifiers come from the core, including unsaved nested projects. */
  async newCardId(name: string, scope: string[] = []): Promise<string> {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) throw new Error('unknown Card scope')
    let id: string
    if (this.cardIdProvider) id = await this.cardIdProvider(name, prefix)
    else {
      if (!this.client) throw new RPCError('disconnected', L('接続していません'))
      id = (await this.client.cardId(name, prefix)).id as string
    }
    if (this.projectPrefix(scope) !== prefix) throw new Error('Card scope changed')
    return id
  }

  /** The host saves the definition and placement together. */
  async addCard(t: DefinitionTemplate, scope: string[], at: { x: number; y: number }, name?: string, automaticPlacement = false): Promise<CardKey> {
    const id = await this.newCardId(name ?? templateBaseName(t), scope)
    await this.operate(H.ProjectOperationBody.ProjectCreateCard, new H.ProjectCreateCardT(
      (this.projectPrefix(scope) ?? '').replace(/\/$/, ''), id, BUILTIN_CARDS[t], at.x, at.y, automaticPlacement))
    this.forgetResult(key([...scope, id]))
    return key([...scope, id])
  }

  async copyTemplate(entry: H.TemplateEntryT, scope: string[], at: { x: number; y: number }) {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) throw new Error('Unknown Card scope')
    return this.operate(H.ProjectOperationBody.ProjectTemplateCopy,
      new H.ProjectTemplateCopyT(prefix.replace(/\/$/, ''), entry.store, entry.name, at.x, at.y))
  }

  async listTemplates(): Promise<H.TemplateEntryT[]> {
    const result = await hostCall<H.TemplateListT>(H.RequestPayload.TemplateListRequest,
      new H.TemplateListRequestT(this.projectId), H.ReplyPayload.TemplateList)
    return result.entries
  }

  async addTemplate(store: H.TemplateStore, addition: H.TemplateAddT) {
    if (store === H.TemplateStore.Project)
      await this.operate(H.ProjectOperationBody.TemplateAdd, addition)
    else
      await hostCall(H.RequestPayload.HostTemplateAddRequest,
        new H.HostTemplateAddRequestT(addition), H.ReplyPayload.Acknowledgement)
  }

  async placeExternalCopy(from: string, scope: string[], at: { x: number; y: number }): Promise<CardKey | undefined> {
    const base = from.split('/').filter(Boolean).pop() ?? 'sub'
    const id = await this.newCardId(base, scope)
    try {
      await this.operate(H.ProjectOperationBody.ProjectCopy, new H.ProjectCopyT(
        (this.projectPrefix(scope) ?? '').replace(/\/$/, ''), [new H.ProjectCopyItemT(from, id, at.x, at.y)]))
      this.forgetResult(key([...scope, id]))
      return key([...scope, id])
    } catch (e) { this.cardEditFailed(e); return undefined }
  }

  async placeDefinitionCopy(prefix: string, id: string, scope: string[], at: { x: number; y: number }): Promise<CardKey | undefined> {
    const newId = await this.newCardId(id, scope)
    try {
      await this.operate(H.ProjectOperationBody.ProjectCopy, new H.ProjectCopyT(
        (this.projectPrefix(scope) ?? '').replace(/\/$/, ''), [new H.ProjectCopyItemT(`${prefix}definitions/${id}/`, newId, at.x, at.y)]))
      this.forgetResult(key([...scope, newId]))
      return key([...scope, newId])
    } catch (e) { this.cardEditFailed(e); return undefined }
  }

  async importProject(folder: string, scope: string[], at: { x: number; y: number }): Promise<CardKey> {
    const id = await this.newCardId(folder.split('/').filter(Boolean).pop() ?? 'sub', scope)
    await this.operate(H.ProjectOperationBody.ProjectImport, new H.ProjectImportT(
      (this.projectPrefix(scope) ?? '').replace(/\/$/, ''), folder, id, at.x, at.y))
    this.forgetResult(key([...scope, id]))
    return key([...scope, id])
  }

  /** A duplicate selection, its wires and positions are one host operation. */
  async duplicate(keys: Set<CardKey>, scope: string[]): Promise<Set<CardKey>> {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined || [...keys].some(k => key(path(k).slice(0, -1)) !== key(scope))) return new Set()
    const items: H.ProjectCopyItemT[] = []
    for (const k of [...keys].sort()) {
      const id = path(k).pop()!
      items.push(new H.ProjectCopyItemT(`${prefix}definitions/${id}/`, await this.newCardId(this.displayName(k), scope)))
    }
    if (!items.length) return new Set()
    try {
      await this.operate(H.ProjectOperationBody.ProjectCopy, new H.ProjectCopyT(prefix.replace(/\/$/, ''), items, true))
      for (const item of items) this.forgetResult(key([...scope, String(item.id)]))
      return new Set(items.map(item => key([...scope, String(item.id)])))
    } catch (e) { this.cardEditFailed(e); return new Set() }
  }

  /** A new Card must not display the result or run state of a previous owner of its ID. */
  private forgetResult(k: CardKey) {
    this.liveViews.get(k)?.clear()
    this.cardStates.delete(k)
    this.changed()
  }

  // ---- A Draft written on the canvas -------------------------------------------------

  /** Whether a Draft has no name of its own: its id is one made for it. */
  draftUntitled(k: CardKey): boolean {
    return this.kind(k) === 'draft' && this.displayName(k) === 'draft'
  }

  /** Starts writing a Draft's content on its Card. */
  async startDraftEdit(k: CardKey, revision?: RevisionEdit) {
    if (!revision && this.inlineDraft) {
      if (this.inlineDraft.card === k) return
      await this.finishDraftEdit()
      if (this.inlineDraft) return
    }
    revision ??= new RevisionEdit()
    const content = str(get(this.cardMeta(k), 'content')) ?? ''
    revision.reload(this.rev)
    this.inlineDraft = { card: k, content, initialContent: content, initialReadme: splitCardNotes(this.cardMeta(k)).readme, revision }
    this.changed()
  }

  setDraftEdit(change: Partial<Pick<InlineDraft, 'content'>>) {
    if (!this.inlineDraft) return
    this.inlineDraft = { ...this.inlineDraft, ...change }
    this.changed()
  }

  /** Saves the Draft text and keeps the edit open if saving fails. */
  async finishDraftEdit(overwrite = false): Promise<void> {
    if (this.draftFinishTask) return this.draftFinishTask
    if (!this.inlineDraft) return
    const task = (async () => {
      do {
        await this.finishDraftEditOnce(overwrite)
        const current = this.inlineDraft
        if (!current || current.revision.conflictRev !== undefined || current.revision.error) break
      } while (true)
    })()
    this.draftFinishTask = task
    try { return await task } finally { this.draftFinishTask = undefined; this.changed() }
  }

  private async finishDraftEditOnce(overwrite: boolean): Promise<void> {
    const e = this.inlineDraft
    if (!e) return
    if (!this.instance(e.card) || e.content === e.initialContent) {
      this.inlineDraft = undefined; this.changed(); return
    }
    const dir = this.definitionDir(e.card)
    if (!dir) return
    // The README.md as it was when writing began (or as the conflict found it).
    const base = e.revision.jsonBase(dir + 'README.md', e.initialReadme, overwrite)
    const before = typeof base === 'string' ? base : undefined
    const readme = parseReadme(before ?? '')
    const after = printReadme({ ...readme, body: e.content })
    const ok = await e.revision.apply(this, x => x.appendEdits([x.jsonEdit(dir + 'README.md', before, after)]))
    if (ok && this.inlineDraft?.revision === e.revision) {
      if (this.inlineDraft.content === e.content) this.inlineDraft = undefined
      else this.inlineDraft = { ...this.inlineDraft, initialContent: e.content, initialReadme: after }
    }
    this.changed()
  }

  deleteCards(keys: Set<CardKey>, scope: string[]) {
    this.selectOperation(H.ProjectSelectionAction.Delete, scope, keys)
  }

  async connect(from: [CardKey, string], to: [CardKey, string], scope: string[]) {
    const fromId = path(from[0]).pop(), toId = path(to[0]).pop(), prefix = this.projectPrefix(scope)
    if (!fromId || !toId || prefix === undefined || !this.isEditable) return
    await this.operate(H.ProjectOperationBody.ProjectConnect, new H.ProjectConnectT(prefix.replace(/\/$/, ''), fromId, from[1], toId, to[1]))
  }

  removeConnection(index: number, scope: string[]) {
    const wire = this.connections(scope)[index]
    if (wire) this.removeConnections([wire], scope)
  }

  removeConnections(list: J[], scope: string[]) {
    if (list.length) this.selectOperation(H.ProjectSelectionAction.RemoveConnections, scope, [], [], list)
  }

  disconnect(k: CardKey, inputs: boolean, scope: string[]) {
    this.selectOperation(inputs ? H.ProjectSelectionAction.DisconnectInputs : H.ProjectSelectionAction.DisconnectOutputs, scope, [k])
  }

  setCollapsed(keys: Iterable<CardKey>, value: boolean) {
    const targets = [...keys]
    if (!targets.length) return
    void this.operateFields(() => targets.flatMap(k => {
      const scope = path(k).slice(0, -1), prefix = this.projectPrefix(scope)
      if (prefix === undefined) return []
      const entry = get(this.layoutOf(scope), 'cards', this.layoutKey(k))
      if (entry === undefined) {
        const at = this.position(k), placed = obj([['x', real(at.x)], ['y', real(at.y)]])
        if (value) placed.set('collapsed', true)
        return [this.fieldEdit(prefix + 'layout.bin', ['cards', this.layoutKey(k)], undefined, placed)]
      }
      return [this.fieldEdit(prefix + 'layout.bin', ['cards', this.layoutKey(k), 'collapsed'], get(entry, 'collapsed'), value ? true : undefined)]
    })).catch(e => this.cardEditFailed(e))
  }

  async setParam(k: CardKey, name: string, literal: string, expectedRev: bigint) {
    const edit = new H.ProjectRewriteParamLiteralT(cardPath(path(k)), name, literal, expectedRev)
    return (await this.operate(H.ProjectOperationBody.ProjectRewriteParamLiteral, edit)).rev ?? this.rev
  }

  /** The host moves the complete selected definition/external closure. */
  async makeSubgraph(keys: Set<CardKey>, scope: string[], name = 'sub'): Promise<CardKey | undefined> {
    const prefix = this.projectPrefix(scope)
    if (!keys.size || prefix === undefined || [...keys].some(k => key(path(k).slice(0, -1)) !== key(scope))) return undefined
    const id = await this.newCardId(name, scope)
    try {
      await this.operate(H.ProjectOperationBody.ProjectMakeSubgraph, new H.ProjectMakeSubgraphT(
        prefix.replace(/\/$/, ''), [...keys].map(k => path(k).pop()!).sort(), id))
      return key([...scope, id])
    } catch (e) { this.cardEditFailed(e); return undefined }
  }

  // ---- Layout: notes and groups --------------------------------------------

  referenceIdProvider?: (kind: string, scope: string, occupied: string[]) => Promise<string>
  private async reserveReference(kind: string, scope = ''): Promise<string> {
    const project = this.projectId
    const occupied: string[] = []
    if (kind === 'comment') occupied.push(...this.comments(scope).map(c => c.id))
    else {
      const queue: string[][] = [[]]
      while (queue.length) {
        const at = queue.shift()!
        for (const item of arr(get(this.layoutOf(at), kind === 'note' ? 'notes' : 'groups')) ?? []) {
          const id = str(get(item, 'id'))
          if (id) occupied.push(id)
        }
        queue.push(...this.childScopes(at))
      }
    }
    if (this.referenceIdProvider) return this.referenceIdProvider(kind, scope, occupied)
    if (!project) throw new RPCError('disconnected', L('接続していません'))
    const result = await hostCall<H.IdentifierT>(H.RequestPayload.IdentifierReserveRequest,
      new H.IdentifierReserveRequestT(project, kind, scope, occupied), H.ReplyPayload.Identifier)
    if (this.projectId !== project || !this.connected) throw new RPCError('disconnected', L('接続していません'))
    return s(result.id)
  }

  notes(scope: string[]): J[] {
    return (arr(get(this.layoutOf(scope), 'notes')) ?? []).filter((n) => (str(get(n, 'scope')) ?? '') === '')
  }

  async addNote(scope: string[], p: { x: number; y: number }): Promise<string> {
    const id = await this.reserveReference('note')
    await this.operateLayoutEntity(scope, 'notes', id, undefined, obj([
      ['id', id], ['scope', ''], ['text', L('メモ')], ['color', 'yellow'],
      ['x', real(p.x)], ['y', real(p.y)], ['width', real(240)], ['height', real(120)],
    ]))
    return id
  }

  /** The scope whose layout holds a note or group (ids are unique). */
  scopeHolding(field: string, id: string): string[] | undefined {
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

  private async operateLayoutEntity(scope: string[], field: string, id: string, oldValue: J | undefined, newValue: J | undefined) {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) return
    await this.operateFields([this.fieldEdit(prefix + 'layout.bin', [field, obj([['id', id]])], oldValue, newValue)])
  }

  updateNote(id: string, fn: (n: JObj) => void) {
    void this.operateFields(() => {
      const scope = this.scopeHolding('notes', id), before = this.note(id)
      if (!scope || !before) throw new Error('Unknown note: ' + id)
      const prefix = this.projectPrefix(scope), after = clone(before) as JObj; fn(after)
      return [this.fieldEdit(prefix + 'layout.bin', ['notes', obj([['id', id]])], before, after)]
    }).catch(e => this.cardEditFailed(e))
  }

  removeNote(id: string) {
    const scope = this.scopeHolding('notes', id), before = this.note(id)
    if (scope && before) void this.operateLayoutEntity(scope, 'notes', id, before, undefined).catch(e => this.cardEditFailed(e))
  }

  groups(scope: string[]): JObj[] {
    return (arr(get(this.layoutOf(scope), 'groups')) ?? []).filter((g): g is JObj => isObj(g) && (str(g.get('scope')) ?? '') === '')
  }

  async addGroup(keys: Set<CardKey>, scope: string[], name = L('グループ')): Promise<string> {
    const id = await this.reserveReference('group')
    await this.operateLayoutEntity(scope, 'groups', id, undefined, obj([
      ['id', id], ['scope', ''], ['name', name], ['color', 'blue'], ['cards', [...keys].map(k => path(k).pop()!).sort()],
    ]))
    return id
  }

  /** Removes the group frame only; its Cards stay where they are. */
  ungroup(id: string) {
    const scope = this.scopeHolding('groups', id)
    if (!scope) return
    const before = this.groups(scope).find(g => str(get(g, 'id')) === id)
    if (before) void this.operateLayoutEntity(scope, 'groups', id, before, undefined).catch(e => this.cardEditFailed(e))
  }

  /** Where a scope's boundary ports are drawn (inside a Card in Card). */
  boundaryPosition(scope: string[], side: string): { x: number; y: number } | undefined {
    const v = get(this.layoutOf(scope), 'boundary', side)
    const x = dbl(get(v, 'x')), y = dbl(get(v, 'y'))
    return x !== undefined && y !== undefined ? { x, y } : undefined
  }

  setBoundaryPosition(scope: string[], side: string, p: { x: number; y: number }) {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) return
    void this.operateFields(() => [this.fieldEdit(prefix + 'layout.bin', ['boundary', side], get(this.layoutOf(scope), 'boundary', side),
      obj([['x', real(Math.round(p.x * 10) / 10)], ['y', real(Math.round(p.y * 10) / 10)]]))]).catch(e => this.cardEditFailed(e))
  }

  // Inferring a missing flow from Card places is presentation of the layout:
  // each UI does it itself rather than asking hostd.
  /** The direction this project's Cards are laid out in (the layout's "flow");
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
    void this.operateFields(() => [this.fieldEdit('layout.bin', ['flow'], get(this.layout, 'flow'), f)]).catch(e => this.cardEditFailed(e))
  }

  layoutHost = layoutHost

  async arrange(scope: string[]) {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) return
    try { await this.operate(H.ProjectOperationBody.ProjectLayout, new H.ProjectLayoutT(prefix.replace(/\/$/, ''), H.ProjectLayoutAction.Arrange, sceneRequest(this.layoutScene(scope)))) }
    catch (error) { this.cardEditFailed(error) }
  }

  /** What the canvas draws in a scope, for tsugumi-hostd to lay out. Sizes and port
   *  positions are those of `CardGeometry`, and Cards are where they are drawn. */
  layoutScene(scope: string[]): LayoutScene {
    const ids = this.cards(scope).map((c) => str(get(c, 'id'))).filter((x): x is string => !!x)
    const cards = ids.map((id) => {
      const k = key([...scope, id]), g = this.geometry(k)
      const ports = (names: string[], output: boolean) => names.map((name) => ({ name, ...g.port(name, output) }))
      return { id, ...this.placedPosition(k), ...g.size, reorderable: ['code', 'input', 'subgraph'].includes(this.kind(k)),
        // Unnamed visual anchors locate Draft wires without declaring Card ports.
        inputs: ports(this.kind(k) === 'draft' ? [''] : g.inputs, false), outputs: ports(this.kind(k) === 'draft' ? [''] : g.outputs, true) }
    })
    const wires = this.connections(scope).flatMap((c) => {
      const from = str(get(c, 'from', 'card')), fromPort = str(get(c, 'from', 'port')), to = str(get(c, 'to', 'card')), toPort = str(get(c, 'to', 'port'))
      return from && fromPort !== undefined && to && toPort !== undefined ? [{ from, fromPort, to, toPort }] : []
    })
    const notes = this.notes(scope).flatMap((n) => {
      const id = str(get(n, 'id'))
      return id ? [{ id, x: dbl(get(n, 'x')) ?? 0, y: dbl(get(n, 'y')) ?? 0, width: dbl(get(n, 'width')) ?? 240, height: dbl(get(n, 'height')) ?? 120 }] : []
    })
    const bar = (side: string): LayoutBar | undefined => {
      if (!scope.length) return undefined
      const list = get(this.boundary(key(scope)), side)
      return {
        ends: isObj(list) ? list.entries().map(([name, v]) => ({ name, card: str(get(v, 'card')) ?? '', port: str(get(v, 'port')) ?? '' })) : [],
        at: this.boundaryPosition(scope, side),
      }
    }
    return { flow: this.flow, grid: prefs.gridSize, snap: prefs.showGrid, cards, wires, notes, inputs: bar('inputs'), outputs: bar('outputs') }
  }


  // ---- Definitions ---------------------------------------------------------------

  removeDefinition(id: string) {
    this.selectOperation(H.ProjectSelectionAction.DeleteDefinitions, [], [key([id])])
  }

  async updateNixPathLock(prefix: string, name?: string) {
    await this.operate(H.ProjectOperationBody.ProjectUpdateNixPathLock,
      new H.ProjectUpdateNixPathLockT(prefix ? prefix.slice(0, -1) : null, name ?? null), false)
  }

  nixPathLock(prefix: string): J | undefined {
    const text = this.carriedText(prefix + 'tsugumi.lock')
    try { return text === undefined ? undefined : parse(text) }
    catch { return undefined }
  }

  async refreshCacheUsage() {
    if (this.closed || !this.connected || !this.client) return
    try {
      await api('/api/runtime/cache-usage', {})
      const usage = await this.client.cacheUsage()
      this.cacheUsage = usage
      this.changed()
    } catch { /* A failed measurement never interrupts execution. */ }
  }

  async clearBuildCache(): Promise<R.ProjectCacheClearResultT> {
    if (!this.client) throw new RPCError('disconnected', L('接続していません'))
    return this.client.clearCache(uuid())
  }

  // ---- Versions (git) ----------------------------------------------------------

  /** The project's versions as last read (tsugumid/runtime/src/versions.cc). */
  versionComments = new Map<string, VersionCommentT[]>()
  versionGraph?: R.VersionGraphT
  /** Why the last read of it failed. */
  versionGraphError?: string
  /** Made or copied by this page: it becomes a repository when it connects. */
  makeRepositoryOnConnect = false

  async versionDiff(commit: string, from?: string) {
    if (!this.projectId) throw new RPCError('disconnected', L('接続していません'))
    return this.projectHost.diff(this.projectId, commit, from)
  }

  async refreshVersions() {
    if (!this.client || !this.projectId) return
    try {
      this.versionGraph = await this.projectHost.graph(this.projectId)
      this.versionGraphError = undefined
    } catch (e) {
      this.versionGraphError = errorText(e)
    }
    this.changed()
  }

  /** A version's Cards and wires (a commit never changes: read once). */
  versionCanvases = new Map<string, H.VersionCanvasT>()
  versionNoteBusy = new Set<string>()
  versionNoteErrors = new Map<string, string>()
  versionNotes = new Map<string, string>()
  private versionNoteRequests = new Map<string, string>()

  async loadVersionCanvas(commit: string) {
    if (!this.client || this.versionCanvases.has(commit)) return
    try {
      this.versionCanvases.set(commit, await this.layoutHost.versionCanvas(await this.client.versionCanvas(commit)))
      this.changed()
    } catch {}
  }

  async loadVersionComments(commit: string) {
    if (!this.projectId || this.versionComments.has(commit)) return
    this.versionComments.set(commit, await this.projectHost.comments(this.projectId, commit))
    this.changed()
  }

  /** Read a version's change record from hostd; no model is called. */
  async loadVersionNote(commit: string) {
    if (!this.client) return
    const request = uuid()
    this.versionNoteRequests.set(commit, request)
    this.versionNoteBusy.add(commit)
    this.versionNoteErrors.delete(commit)
    this.changed()
    try {
      await this.loadVersionComments(commit)
      const { text } = await api<{ text: string }>('/api/versions/note', { path: this.dir, commit })
      if (this.versionNoteRequests.get(commit) === request) this.versionNotes.set(commit, text)
    } catch (e) {
      if (this.versionNoteRequests.get(commit) === request) this.versionNoteErrors.set(commit, String((e as Error)?.message ?? e))
    } finally {
      if (this.versionNoteRequests.get(commit) === request) {
        this.versionNoteBusy.delete(commit)
        this.versionNoteRequests.delete(commit)
      }
      this.changed()
    }
  }

  async makeRepository() {
    if (!this.client) throw new RPCError('disconnected', L('接続していません'))
    await this.client.versionInit(uuid(), 'ui')
    await this.refreshVersions()
  }

  /** Names what is saved now as a version (a commit and an annotated tag). */
  async markVersion(name: string, message: string) {
    if (!this.client) throw new RPCError('disconnected', L('接続していません'))
    if (this.dirty) await this.save()
    if (this.dirty) throw new Error(L('未保存の変更があります。保存してから選んでください。'))
    await this.client.versionMark(uuid(), 'ui', name, message)
    await this.refreshVersions()
  }

  /** Starts from `commit`: the documents become that version's; a change and
   *  run from there opens a branch, and nothing later is lost. */
  async startFrom(commit: string) {
    if (!this.client) throw new RPCError('disconnected', L('接続していません'))
    if (this.dirty) throw new Error(L('未保存の変更があります。保存してから選んでください。'))
    await this.client.versionCheckout(this.rev, uuid(), 'ui', commit)
    await this.refreshVersions()
  }

  async deleteRunScratch(paths: string[]): Promise<R.RunScratchDeleteResultT> {
    if (!this.client) throw new RPCError('disconnected', L('接続していません'))
    const result = await this.client.deleteRunScratch(uuid(), paths)
    await this.refreshCacheUsage()
    return result
  }

  async deleteRunCategory(runId: string, category: R.RunRecordCategory) {
    if (!this.projectId || !this.connected) throw new RPCError('disconnected', L('接続していません'))
    await hostCall<R.RunCategoryDeleteResultT>(H.RequestPayload.RunCategoryDeleteRequest,
      new H.RunCategoryDeleteRequestT(this.projectId, runId, category, uuid()), H.ReplyPayload.RunCategoryDeleteResult)
  }

  async deleteRunHistory(endedBefore: Date | null): Promise<R.RunHistoryDeleteResultT> {
    if (!this.client) throw new RPCError('disconnected', L('接続していません'))
    return this.client.deleteHistory(uuid(), endedBefore ? endedBefore.getTime() : null)
  }

  // ---- Canvas data (CardNode) ---------------------------------------------------------

  /** Ports for drawing: resolved ports when available, else declared inputs. */
  ports(k: CardKey): { inputs: string[]; outputs: string[] } {
    const d = this.portData(k)
    return { inputs: d.inputs.map((p) => p.name), outputs: d.outputs.map((p) => p.name) }
  }

  portData(k: CardKey, definition?: J): { inputs: PortData[]; outputs: PortData[] } {
    const def = definition ?? this.cardDefinition(k)
    const r = this.resolution.get(k)
    if (definitionKind(def) === 'collect') return {
      inputs: [{ name: 'in', typeName: portTypeName(r?.inputs.find((p) => p.name === 'in')) }],
      outputs: [{ name: 'out', typeName: portTypeName(r?.outputs.find((p) => p.name === 'out')) }],
    }
    const kind = definitionKind(def)
    const draft = kind === 'draft'
    if (draft || kind === 'invalid') return { inputs: [], outputs: [] }
    if (definition === undefined && r && !draft) {
      return {
        inputs: r.inputs.map((p) => ({ name: p.name, typeName: portTypeName(p) })),
        outputs: r.outputs.map((p) => ({ name: p.name, typeName: portTypeName(p) })),
      }
    }
    // Not resolved yet (unsaved) or a working copy: declared ports.
    const ins = (arr(get(def, 'inputs')) ?? []).flatMap((i) => {
      const n = str(get(i, 'name'))
      return n ? [{ name: n, typeName: portTypeName(r?.inputs.find((p) => p.name === n)) }] : []
    })
    const outs: PortData[] = (r?.outputs ?? []).map((p) => ({ name: p.name, typeName: portTypeName(p) }))
    return { inputs: ins, outputs: outs }
  }

  /** Scalar parameters editable on Input Cards. */
  inlineParams(k: CardKey): ParamData[] {
    if (this.kind(k) !== 'input') return []
    return (this.resolution.get(k)?.parameters ?? []).filter(p => !p.type.startsWith('['))
      .map(p => ({ name: p.name, type: p.type, value: paramText(p.value) }))
  }

  cardDescription(k: CardKey): string | undefined {
    return str(get(this.cardMeta(k), 'description')) || undefined
  }

  /** What a Card says about itself on hover: a Draft's summary and content. */
  cardDetails(k: CardKey): string[] {
    const desc = this.cardDescription(k)
    const content = this.kind(k) === 'draft' ? str(get(this.cardMeta(k), 'content')) : undefined
    return [desc, content, ...this.openComments(k).map((c) => L('{0}: {1}', c.author === 'agent' ? L('コメント（Agent）') : L('コメント'), c.text))].filter((x): x is string => !!x)
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
        const sources = conns.filter((c) => str(get(c, 'to', 'card')) === id)
          .map((c) => str(get(c, 'from', 'card')) ?? '')
        const blocked = this.kind(key([...scope, id])) === 'merge'
          ? sources.length > 0 && sources.every((f) => waits.has(f))
          : sources.some((f) => waits.has(f))
        if (blocked) {
          waits.add(id)
          changed = true
        }
      }
    }
    return waits
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

  // Presentation: each UI combines the inner Cards' states itself, not hostd.
  /** State of a Card in Card from the Cards inside it. */
  compositeState(k: CardKey): D.CardState | undefined {
    const inner = [...new Set([...this.cardStates.keys(), ...this.runBatches.keys()])]
      .filter((kk) => kk.startsWith(k + '/')).flatMap((kk) => {
        const state = this.canvasCardState(kk)
        return state === undefined ? [] : [state]
      })
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

  boundaryInputSource(scope: string[], port: string): [CardKey, string] | undefined {
    const container = scope.at(-1)
    if (!container) return
    const parent = scope.slice(0, -1)
    const links = this.connections(parent).filter((c) => str(get(c, 'to', 'card')) === container && str(get(c, 'to', 'port')) === port)
    if (links.length !== 1) return
    const from = str(get(links[0], 'from', 'card')), output = str(get(links[0], 'from', 'port'))
    return from && output ? [key([...parent, from]), output] : undefined
  }

  nodeData(k: CardKey, opts: { definition?: J; meta?: J; collapsed?: boolean; outputs?: PortData[] } = {}): CardNodeData {
    const kind = opts.definition === undefined ? this.kind(k) : definitionKind(opts.definition)
    const state = this.cardStates.get(k)
    const batch = this.executionBatch(k)
    const executionList = batch?.executionList === true
    const empty = executionList && batch.complete && !batch.failed && batch.invocationCount === 0
    const diags = this.resolution.get(k)?.diagnostics ?? []
    const ports = this.portData(k, opts.definition)
    const def = opts.definition ?? this.cardDefinition(k)
    const m = opts.meta ?? this.cardMeta(k)
    let summary = str(get(m, 'description'))
    // What a Draft shows on its Card is what it says it will do; a Card whose
    // definition cannot be read, what the core says about it.
    if (kind === 'draft') summary = str(get(m, 'content')) || summary
    if (kind === 'invalid') summary = diags.find((x) => x.error)?.message ?? L('Card の定義を読めません')
    const sym = str(get(def, 'symbol'))
    const editing = kind === 'draft' && this.inlineDraft?.card === k
    const pending = state?.executionState === D.RemoteExecutionState.queued || state?.executionState === D.RemoteExecutionState.disconnected ||
      state?.executionState === D.RemoteExecutionState.cancelling ? remoteExecutionLabel(state.executionState) : undefined
    return {
      title: path(k).pop() || k, kind, runMark: state || batch ? runMark(this.runStatus) : undefined, symbol: kind === 'invalid' ? undefined : sym || undefined, summary: summary || undefined,
      inputs: this.visualPorts(k, ports.inputs, false), outputs: this.visualPorts(k, opts.outputs ?? ports.outputs, true), params: this.inlineParams(k),
      result: state?.transfer ?? pending ?? (!state && kind !== 'draft' && this.waitsOnDraft(k) ? L('Draft のため実行しない') : undefined),
      state: kind === 'subgraph' ? this.compositeState(k) : this.canvasCardState(k), stage: empty ? undefined : this.stageProgress(k), executionList,
      invocation: empty ? 0n : state?.invocation ?? 0n, runContinues: this.runActive,
      failure: state && (state.state === D.CardState.failed || state.state === D.CardState.blocked) ? state.failure ?? cardState(state.state).label : undefined,
      errors: diags.filter((x) => x.error).map((x) => x.message), warnings: diags.filter((x) => !x.error).map((x) => x.message),
      collapsed: !editing && (opts.collapsed ?? this.collapsed(k)), permissions: cardPermissions(def),
      draftEditing: editing, untitled: this.draftUntitled(k), openComments: cardComments(m).filter((c) => !c.resolved).length,
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

  // Shown ports and their order (the layout's inputOrder / outputOrder) are
  // presentation: each UI works them out itself rather than asking hostd.
  private visualPorts(k: CardKey, ports: PortData[], output: boolean): PortData[] {
    if (!['code', 'draft', 'input', 'subgraph'].includes(this.kind(k))) return ports
    const saved = arr(get(this.layoutOf(path(k).slice(0, -1)), 'cards', this.layoutKey(k), output ? 'outputOrder' : 'inputOrder')) ?? []
    return orderedPorts(ports, saved.flatMap(v => typeof v === 'string' ? [v] : []))
  }

  /** Keep the layout while reducing crossings (LayoutUntangle); one undo restores every port. */
  async untangle(scope: string[]) {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) return
    try { await this.operate(H.ProjectOperationBody.ProjectLayout, new H.ProjectLayoutT(prefix.replace(/\/$/, ''), H.ProjectLayoutAction.Untangle, sceneRequest(this.layoutScene(scope)))) }
    catch (error) { this.cardEditFailed(error) }
  }

  async moveSelection(scope: string[], cards: Set<CardKey>, notes: Set<string>, dx: number, dy: number, snap = 0): Promise<void> {
    const prefix = this.projectPrefix(scope)
    if (prefix === undefined) return
    const local = (keys: Set<CardKey>) => [...keys].map(k => this.layoutKey(k)).sort()
    try {
      await this.operate(H.ProjectOperationBody.ProjectMove, new H.ProjectMoveT(prefix.replace(/\/$/, ''), local(cards), [...notes].sort(), dx, dy, snap, sceneRequest(this.layoutScene(scope))))
    } catch (e) { this.cardEditFailed(e) }
  }

  /** Whether the canvas shows this Card's ports in an order of its own. */
  hasPortOrder(k: CardKey): boolean {
    const entry = get(this.layoutOf(path(k).slice(0, -1)), 'cards', this.layoutKey(k))
    return !!(arr(get(entry, 'inputOrder'))?.length || arr(get(entry, 'outputOrder'))?.length)
  }

  /** Back to the declared order of the ports, in one undo step. */
  async resetPortOrder(keys: Iterable<CardKey>) {
    const targets = [...keys].filter(k => this.hasPortOrder(k))
    if (!targets.length) return
    const edits: H.ProjectEditT[] = []
    for (const k of targets) {
      const scope = path(k).slice(0, -1), prefix = this.projectPrefix(scope)
      if (prefix === undefined) continue
      const entry = get(this.layoutOf(scope), 'cards', this.layoutKey(k))
      for (const field of ['inputOrder', 'outputOrder']) {
        const value = get(entry, field)
        if (value !== undefined) edits.push(this.fieldEdit(`${prefix}layout.bin`, ['cards', this.layoutKey(k), field], value, undefined))
      }
    }
    try { await this.operateFields(edits) } catch (error) { this.cardEditFailed(error) }
  }



  geometry(k: CardKey): CardGeometry {
    return new CardGeometry(this.nodeData(k), this.flow)
  }
}

/** An external's documents and files, ready to be written under another folder. */


// ---- Kinds, templates and labels ------------------------------------------------------

/** What a Card is, for choosing what the canvas and the editor offer.
 *  `invalid`: its _card.json has no type or one this build does not know (or
 *  one that does not match `external`), a definition error the core reports. */
export type CardKind = 'trigger' | 'input' | 'merge' | 'collect' | 'draft' | 'subgraph' | 'code' | 'invalid'
/** The _card.json "type" names. */
const KINDS: CardKind[] = ['trigger', 'input', 'merge', 'collect', 'draft', 'subgraph', 'code']

export const KIND_LABEL: Record<CardKind, string> = {
  trigger: L('開始'), input: L('入力'), merge: L('合流'), collect: L('収集'),
  draft: 'Draft', subgraph: 'Card in Card', code: L('処理'), invalid: L('定義エラー'),
}

export const KIND_ICON: Record<CardKind, string> = {
  trigger: 'play.circle', input: 'slider.horizontal.3',
  merge: 'arrow.triangle.merge', collect: 'line.3.horizontal.decrease', draft: 'pencil.and.outline', subgraph: 'link',
  code: 'chevron.left.forwardslash.chevron.right', invalid: 'xmark.octagon',
}

/** A kind by its _card.json "type" name (also VersionCanvasCard.kind, empty
 *  for a definition the daemon cannot read). Anything else is `invalid`: the
 *  Card shows as a definition error, with no ports and no run. */
export const cardKindNamed = (name: string | undefined): CardKind =>
  name !== undefined && (KINDS as string[]).includes(name) ? name as CardKind : 'invalid'

/** What a _card.json says the Card is: its "type" (a subgraph exactly when it
 *  names an `external`). Without a definition, `invalid` as well. */
export function definitionKind(def: J | undefined): CardKind {
  const kind = cardKindNamed(str(get(def, 'type')))
  return (kind === 'subgraph') === (get(def, 'external') !== undefined) ? kind : 'invalid'
}

const BUILTIN_CARDS = {
  draft: H.BuiltinCard.Draft,
  trigger: H.BuiltinCard.Trigger,
  input: H.BuiltinCard.InputValue,
  input_bool: H.BuiltinCard.BooleanInput,
  merge: H.BuiltinCard.Merge,
  collect: H.BuiltinCard.Collect,
}
export type DefinitionTemplate = keyof typeof BUILTIN_CARDS
export const TEMPLATES: { id: DefinitionTemplate; label: string }[] = [
  { id: 'draft', label: L('Draft') },
  { id: 'trigger', label: L('トリガー') },
  { id: 'input', label: L('入力値') },
  { id: 'input_bool', label: L('真偽値の入力') },
  { id: 'merge', label: L('合流') },
  { id: 'collect', label: L('収集') },
]
export const templateBaseName = (t: DefinitionTemplate) => t === 'input_bool' ? 'condition' : t === 'trigger' ? 'start' : t

export const NOTE_COLORS: { id: string; label: string }[] = [
  { id: 'yellow', label: L('黄') }, { id: 'blue', label: L('青') }, { id: 'green', label: L('緑') }, { id: 'pink', label: L('桃') }, { id: 'gray', label: L('灰') },
]

export const RUN_STATE: Record<number, { label: string; icon: string; color: string }> = {
  [D.RunState.idle]: { label: L('待機中'), icon: 'circle', color: 'secondary' },
  [D.RunState.queued]: { label: L('待機列'), icon: 'hourglass', color: 'orange' },
  [D.RunState.preparing]: { label: L('準備中'), icon: 'hourglass', color: 'orange' },
  [D.RunState.running]: { label: L('実行中'), icon: 'play.circle.fill', color: 'orange' },
  [D.RunState.cancelling]: { label: L('停止中'), icon: 'stop.circle', color: 'orange' },
  [D.RunState.succeeded]: { label: L('成功'), icon: 'checkmark.circle.fill', color: 'green' },
  [D.RunState.failed]: { label: L('失敗'), icon: 'xmark.circle.fill', color: 'red' },
  [D.RunState.cancelled]: { label: L('停止済み'), icon: 'stop.circle.fill', color: 'brown' },
}
export const runState = (st: D.RunState | undefined) => RUN_STATE[st ?? D.RunState.idle] ?? { label: L('未実行'), icon: 'circle', color: 'secondary' }

export const CARD_STATE: Record<number, { label: string; icon: string; color: string }> = {
  [D.CardState.not_started]: { label: L('未実行'), icon: 'circle.dotted', color: 'secondary' },
  [D.CardState.waiting]: { label: L('入力待ち'), icon: 'hourglass', color: 'orange' },
  [D.CardState.ready]: { label: L('実行待ち'), icon: 'hourglass', color: 'orange' },
  [D.CardState.preparing]: { label: L('準備中'), icon: 'gearshape', color: 'orange' },
  [D.CardState.running]: { label: L('実行中'), icon: 'play.circle.fill', color: 'orange' },
  [D.CardState.succeeded]: { label: L('成功'), icon: 'checkmark.circle.fill', color: 'green' },
  [D.CardState.failed]: { label: L('失敗'), icon: 'xmark.circle.fill', color: 'red' },
  [D.CardState.blocked]: { label: L('ブロック'), icon: 'nosign', color: 'purple' },
  [D.CardState.cancelled]: { label: L('停止済み'), icon: 'stop.circle.fill', color: 'brown' },
  [D.CardState.skipped]: { label: L('スキップ'), icon: 'arrow.uturn.right.circle', color: 'gray' },
}
export const cardState = (st: D.CardState) => CARD_STATE[st] ?? { label: L('不明'), icon: 'circle.dotted', color: 'secondary' }

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
export function parameterInputLiteral(text: string, type: string): string {
  if (type.startsWith('[')) return text
  return parameterLiteral(type === 'string' ? text : tryParse(text) ?? text, type)
}

export const paramText = (v: J | undefined, type = ''): string => {
  if (Array.isArray(v)) return parameterLiteral(v, type)
  if (typeof v === 'string') return v
  const n = numText(v)
  if (n !== undefined) return n
  if (typeof v === 'boolean') return v ? 'true' : 'false'
  return ''
}
