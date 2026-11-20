// Typed project operations. The shared C++ client owns transport, validation,
// request correlation, deadlines and disconnect handling (Wasm in the browser).
import { Builder, ByteBuffer, Encoding } from 'flatbuffers'
import * as D from '../../shared/gen/tsugumi/data'
import * as R from '../../shared/gen/tsugumi/rpc'
import { Message, MessageT } from '../../shared/gen/tsugumi/rpc/message'
import { Payload } from '../../shared/gen/tsugumi/rpc/payload'
import { Reply } from '../../shared/gen/tsugumi/rpc/reply'
import { ReplyT } from '../../shared/gen/tsugumi/rpc/reply'
import { RequestPayload } from '../../shared/gen/tsugumi/rpc/request-payload'
import { RequestT } from '../../shared/gen/tsugumi/rpc/request'
import { NotificationT } from '../../shared/gen/tsugumi/rpc/notification'
import { L } from '../i18n'
import { Client } from '../../shared/gen/tsugumi/trace/client'

export { D, R, RequestPayload }

/** Strings of the object API are always decoded text. */
export const s = (v: string | Uint8Array | null | undefined): string => (typeof v === 'string' ? v : v ? new TextDecoder().decode(v) : '')
export const sOpt = (v: string | Uint8Array | null | undefined): string | undefined => (v === null || v === undefined ? undefined : s(v))

/** Preserve invalid display text as an unavailable display, before lossy decoding. */
export function unpackDisplay(wire: D.Display): D.DisplayT {
  return wire.unpack()
}

function unpackRecordResult(wire: R.RecordResult): R.RecordResultT {
  const value = Object.assign(new R.RecordResultT(), {
    selectedInvocation: wire.selectedInvocation(), run: wire.run()?.unpack() ?? null,
    status: wire.status()?.unpack() ?? null, aggregate: wire.aggregate()?.unpack() ?? null,
    exists: wire.exists(), recordingDisabled: wire.recordingDisabled(), truncated: wire.truncated(), orphaned: wire.orphaned(),
  })
  const record = wire.record()
  if (record) value.record = Object.assign(new R.InvocationRecordT(), {
    summary: record.summary()?.unpack() ?? null,
    inputs: Array.from({ length: record.inputsLength() }, (_, i) => record.inputs(i)!.unpack()),
    outputs: Array.from({ length: record.outputsLength() }, (_, i) => record.outputs(i)!.unpack()),
    inputSources: Array.from({ length: record.inputSourcesLength() }, (_, i) => record.inputSources(i)!.unpack()),
    display: record.display() ? unpackDisplay(record.display()!) : null,
    recordIo: record.recordIo(), paramsJson: record.paramsJson(),
  })
  return value
}

/** A structured failure returned by tsugumid (never matched by its text). */
export class RPCFailure extends Error {
  readonly kind: D.FailureKind
  readonly args: Record<string, string>
  readonly permissions: string[]
  readonly diagnostic?: string
  readonly currentRev?: bigint
  readonly retryAfterMs?: number

  constructor(f: { kind: D.FailureKind; args?: D.KeyValueT[]; diagnostic?: string | Uint8Array | null; currentRev?: bigint | null; retryAfterMs?: number | null }) {
    super(`${D.FailureKind[f.kind] ?? f.kind}: ${s(f.diagnostic)}`)
    this.kind = f.kind
    this.permissions = (f.args ?? []).filter(kv => s(kv.key) === 'permission').map(kv => s(kv.value))
    this.args = {}
    for (const kv of f.args ?? []) this.args[s(kv.key)] = s(kv.value)
    this.diagnostic = sOpt(f.diagnostic)
    this.currentRev = f.currentRev ?? undefined
    this.retryAfterMs = f.retryAfterMs ?? undefined
  }

  get kindName(): string {
    return D.FailureKind[this.kind] ?? String(this.kind)
  }
}

export class RPCError extends Error {
  constructor(readonly code: 'disconnected' | 'protocol' | 'timeout' | 'unexpected', message: string) {
    super(message)
  }
}

export const disconnected = () => new RPCError('disconnected', L('tsugumid との接続が切れました'))

export interface Transport {
  /** Resolves with the verified reply matched to the request ID. */
  call(frame: Uint8Array, timeoutMs: number): Promise<Uint8Array>
  close(): void
  /** Set by the client. The transport delivers verified notifications here. */
  onEvent?: (frame: Uint8Array) => void
  onClose?: (reason: string) => void
}

export interface NotificationEvent {
  subscriptionId: string
  seq: bigint
  type: R.NotificationPayload
  value: NotificationT['value']
}

export const uuid = (): string =>
  typeof crypto !== 'undefined' && 'randomUUID' in crypto
    ? crypto.randomUUID()
    : 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, (c) => {
        const r = (Math.random() * 16) | 0
        return (c === 'x' ? r : (r & 0x3) | 0x8).toString(16)
      })

export class RPCClient {
  private nextId = 1n
  private _closed = false
  hello?: R.HelloResultT
  onNotification?: (e: NotificationEvent) => void
  onClose?: (reason: string) => void

  constructor(private readonly transport: Transport) {
    transport.onEvent = (f) => this.receiveEvent(f)
    transport.onClose = (reason) => this.closedBy(reason)
  }

  get closed() {
    return this._closed
  }

  /** SessionHello: must be the first request. The token never appears in URLs. */
  async connect(token: string | undefined, expectedProjectId?: string): Promise<R.HelloResultT> {
    const req = Object.assign(new R.SessionHelloRequestT(), { token: token ?? null, expectedProjectId: expectedProjectId ?? null, client: Client.Web })
    this.hello = await this.call<R.HelloResultT>(RequestPayload.SessionHelloRequest, req)
    return this.hello
  }

  close() {
    if (this._closed) return
    this._closed = true
    this.transport.close()
  }

  private closedBy(reason: string) {
    if (this._closed) return
    this._closed = true
    this.onClose?.(reason)
  }

  private receiveEvent(frame: Uint8Array) {
    try {
      const bb = new ByteBuffer(frame)
      const message = Message.getRootAsMessage(bb)
      const raw = message.payload(new R.Notification()) as R.Notification
      const note = raw.valueType() === R.NotificationPayload.DisplayUpdated
        ? Object.assign(new R.NotificationT(), { subscriptionId: raw.subscriptionId(), seq: raw.seq(), valueType: raw.valueType(),
          value: new R.DisplayUpdatedT(unpackDisplay((raw.value(new R.DisplayUpdated()) as R.DisplayUpdated).display()!)) })
        : raw.unpack()
      this.onNotification?.({ subscriptionId: s(note.subscriptionId), seq: note.seq ?? 0n, type: note.valueType, value: note.value })
    } catch (error) {
      this.transport.close()
      this.closedBy(String(error))
    }
  }

  private async dispatch(frame: Uint8Array, timeoutMs: number): Promise<ReplyT> {
    if (this._closed) throw disconnected()
    const bytes = await this.transport.call(frame, timeoutMs)
    // The native/Wasm library has already verified the entire FlatBuffer and ID.
    const message = Message.getRootAsMessage(new ByteBuffer(bytes))
    const wire = message.payload(new Reply()) as Reply
    const type = wire.valueType()
    if (type === R.ReplyPayload.RecordResult)
      return new ReplyT(type, unpackRecordResult(wire.value(new R.RecordResult()) as R.RecordResult))
    if (type === R.ReplyPayload.LogSlice) {
      const slice = wire.value(new R.LogSlice()) as R.LogSlice
      return new ReplyT(type, new R.LogSliceT(
        slice.text(Encoding.UTF8_BYTES), slice.offset(), slice.nextOffset(), slice.sizeBytes(),
        slice.exists(), slice.truncated(), slice.logTruncatedAtBytes(), slice.lineNumber(), slice.startsLine(),
        slice.rawSizeBytes(), slice.reset(),
      ))
    }
    return wire.unpack()
  }

  /** Sends one request and waits for its reply. Failure replies throw RPCFailure. */
  async call<T>(type: RequestPayload, value: RequestT['value'], timeoutMs = 0): Promise<T> {
    const id = this.nextId++
    const msg = new MessageT(id, Payload.Request, new RequestT(type, value))
    const b = new Builder(1024)
    b.finish(msg.pack(b), 'TSG1')
    const reply = await this.dispatch(b.asUint8Array().slice(), timeoutMs)
    if (reply.valueType === R.ReplyPayload.Failure) throw new RPCFailure(reply.value as D.FailureT)
    return reply.value as T
  }

  cardId(name: string, projectPath = '') {
    return this.call<R.CardIdResultT>(RequestPayload.CardIdRequest, Object.assign(new R.CardIdRequestT(), { stem: cardStem(name), projectPath }))
  }

  // ---- Typed wrappers ---------------------------------------------------------

  open(viewId: string | null) {
    return this.call<R.OpenResultT>(RequestPayload.ProjectOpenRequest, Object.assign(new R.ProjectOpenRequestT(), { viewId }))
  }
  inspect() {
    return this.call<R.ProjectSnapshotT>(RequestPayload.ProjectInspectRequest, new R.ProjectInspectRequestT())
  }
  save(baseRev: bigint, opId: string, origin: string, changes: R.DocumentChangeT[], checkPermissions = false, approvedPermissions: string[] = []) {
    return this.call<R.SaveResultT>(RequestPayload.ProjectSaveRequest, Object.assign(new R.ProjectSaveRequestT(), { baseRev, opId, origin, changes, checkPermissions, approvedPermissions }))
  }
  /** Generates the Card's contracts (_inputs.fbs, _outputs.fbs, _params.fbs)
   *  for one revision and returns the file list when ready. */
  async generatedContracts(card: string[], atRev: bigint): Promise<R.SDKFileT[]> {
    let res = await this.call<R.CardSDKPrepareResultT>(RequestPayload.CardSDKPrepareRequest,
      Object.assign(new R.CardSDKPrepareRequestT(), { card: cardPath(card), atRev, opId: uuid() }))
    for (let i = 0; i < 100 && res.state === D.SDKState.pending; i++) {
      await new Promise((r) => setTimeout(r, 100))
      res = await this.call<R.CardSDKPrepareResultT>(RequestPayload.CardSDKStatusRequest, Object.assign(new R.CardSDKStatusRequestT(), { opId: res.opId }))
    }
    return res.files
  }
  /** Deletes finished runs (all, or those that ended before the date) with their files. */
  deleteHistory(opId: string, endedBefore: number | null) {
    return this.call<R.RunHistoryDeleteResultT>(RequestPayload.RunHistoryDeleteRequest,
      Object.assign(new R.RunHistoryDeleteRequestT(), { opId, endedBefore: endedBefore === null ? null : BigInt(endedBefore) }))
  }
  /** The types of an _outputs.fbs text as the core compiles them (bfbs); with
   *  a port and a type, also the text with that port changed. Saves nothing. */
  outputTypes(text: string, port: string | null = null, typeName: string | null = null, kind: D.PortKind = D.PortKind.FlatBuffer) {
    return this.call<R.OutputTypesResultT>(RequestPayload.OutputTypesRequest,
      Object.assign(new R.OutputTypesRequestT(), { text, port, typeName, kind: port !== null ? kind : D.PortKind.unspecified }))
  }
  /** Checks the document as it would be after `changes` (nothing is saved). */
  validate(changes: R.DocumentChangeT[] | null = null) {
    return this.call<R.ProjectValidateResultT>(RequestPayload.ProjectValidateRequest,
      Object.assign(new R.ProjectValidateRequestT(), { mode: D.ValidateMode.executable, changes: changes ?? [] }))
  }
  operation(opId: string) {
    return this.call<R.OperationResultT>(RequestPayload.OperationGetRequest, Object.assign(new R.OperationGetRequestT(), { opId }))
  }

  /** Reads a whole file in chunks at the selected revision. */
  async readFile(path: string, atRev: bigint): Promise<Uint8Array> {
    const parts: Uint8Array[] = []
    let offset = 0n
    for (;;) {
      const r = Object.assign(new R.ProjectFileReadRequestT(), { path, atRev, offset, limitBytes: 1 << 20 })
      const c = await this.call<R.FileContentT>(RequestPayload.ProjectFileReadRequest, r)
      parts.push(Uint8Array.from(c.bytes))
      offset = c.nextOffset ?? offset
      if (c.eof) break
    }
    const out = concat(parts)
    return out
  }

  run(baseRev: bigint, opId: string, card: string[], propagate: boolean, maxConcurrency = 4, origin = 'ui') {
    return this.call<R.RunAcceptedT>(
      RequestPayload.RunRequest,
      Object.assign(new R.RunRequestT(), { baseRev, opId, origin, card: cardPath(card), propagate, maxConcurrency }),
    )
  }
  cancel(runId: string) {
    return this.call<R.RunStatusT>(RequestPayload.RunCancelRequest, Object.assign(new R.RunCancelRequestT(), { runId }))
  }
  history(card: string[] | null = null, artifactsOnly = false, limit = 50, cursor: string | null = null) {
    return this.call<R.RunPageT>(
      RequestPayload.RunHistoryRequest,
      Object.assign(new R.RunHistoryRequestT(), { card: card ? cardPath(card) : null, artifactsOnly, cursor, limit }),
    )
  }
  invocations(runId: string, card: string[], cursor: string | null = null) {
    return this.call<R.InvocationPageT>(
      RequestPayload.RunInvocationsRequest,
      Object.assign(new R.RunInvocationsRequestT(), { runId, card: cardPath(card), cursor, limit: 500 }),
    )
  }
  record(runId: string, card: string[], invocation: bigint | null = null, followLatest = false) {
    return this.call<R.RecordResultT>(
      RequestPayload.RunRecordRequest,
      Object.assign(new R.RunRecordRequestT(), { runId, card: cardPath(card), invocation, followLatest }),
    )
  }
  logRange(runId: string, card: string[] | null, invocation: bigint | null, stream: D.LogStream, offset = 0n, tail = false, limit = 65536, before: bigint | null = null, terminal = false, terminalSince: bigint | null = null) {
    return this.call<R.LogSliceT>(
      RequestPayload.LogRangeRequest,
      Object.assign(new R.LogRangeRequestT(), {
        runId, card: card ? cardPath(card) : null, invocation, stream, offset: tail || before !== null ? null : offset, tail, limitBytes: limit, before, terminal, terminalSince,
      }),
    )
  }
  async artifacts(runId: string, card: string[] | null = null): Promise<R.ArtifactInfoT[]> {
    const out: R.ArtifactInfoT[] = []
    let cursor: string | null = null
    do {
      const r: R.ArtifactListRequestT = Object.assign(new R.ArtifactListRequestT(), { runId, card: card ? cardPath(card) : null, cursor, limit: 500 })
      const page: R.ArtifactPageT = await this.call<R.ArtifactPageT>(RequestPayload.ArtifactListRequest, r)
      out.push(...page.artifacts)
      cursor = sOpt(page.nextCursor) ?? null
    } while (cursor)
    return out
  }
  async readArtifact(artifactId: string, member: string | null): Promise<Uint8Array> {
    const parts: Uint8Array[] = []
    let offset = 0n
    for (;;) {
      const r = Object.assign(new R.ArtifactReadRequestT(), { artifactId, member, offset })
      const c = await this.call<R.FileContentT>(RequestPayload.ArtifactReadRequest, r)
      parts.push(Uint8Array.from(c.bytes))
      offset = c.nextOffset ?? offset
      if (c.eof) break
    }
    const out = concat(parts)
    return out
  }
  setVisible(subscriptionId: string, cards: string[][], enabled = true) {
    return this.call<R.AckT>(
      RequestPayload.ViewSetVisibleRequest,
      Object.assign(new R.ViewSetVisibleRequestT(), { subscriptionId, cards: cards.map(cardPath), enabled }),
    )
  }
  saveView(viewId: string, baseViewRev: bigint, view: string) {
    return this.call<R.ViewSaveResultT>(RequestPayload.ViewSaveRequest, Object.assign(new R.ViewSaveRequestT(), { viewId, baseViewRev, viewJson: view }))
  }
  /** Each Card's latest invocation of a run. */
  latestInvocations(runId: string) {
    return this.call<R.InvocationPageT>(RequestPayload.RunLatestInvocationsRequest, Object.assign(new R.RunLatestInvocationsRequestT(), { runId }))
  }
  /** Looks a flake up for nixSources (nothing is saved). */
  resolveNixSource(ref: string) {
    return this.call<R.NixSourceResolvedT>(RequestPayload.NixSourceResolveRequest, Object.assign(new R.NixSourceResolveRequestT(), { ref }))
  }
  /** Deletes the project's build caches; documents and history are kept. */
  clearCache(opId: string) {
    return this.call<R.ProjectCacheClearResultT>(RequestPayload.ProjectCacheClearRequest, Object.assign(new R.ProjectCacheClearRequestT(), { opId }))
  }
  deleteRunScratch(opId: string, paths: string[]) {
    return this.call<R.RunScratchDeleteResultT>(RequestPayload.RunScratchDeleteRequest, new R.RunScratchDeleteRequestT(opId, paths))
  }
  cacheUsage() {
    return this.call<R.ProjectCacheUsageResultT>(RequestPayload.ProjectCacheUsageRequest, new R.ProjectCacheUsageRequestT())
  }
  // ---- Versions (git): tsugumid/runtime/src/versions.cc
  versionGraph(limit = 500) {
    return this.call<R.VersionGraphT>(RequestPayload.VersionGraphRequest, Object.assign(new R.VersionGraphRequestT(), { limit }))
  }
  versionCanvas(commit: string) {
    return this.call<R.VersionCanvasT>(RequestPayload.VersionCanvasRequest, Object.assign(new R.VersionCanvasRequestT(), { commit }))
  }

  versionInit(opId: string, origin: string) {
    return this.call<R.VersionResultT>(RequestPayload.VersionInitRequest, Object.assign(new R.VersionInitRequestT(), { opId, origin }))
  }
  versionMark(opId: string, origin: string, name: string, message: string) {
    return this.call<R.VersionResultT>(RequestPayload.VersionMarkRequest, Object.assign(new R.VersionMarkRequestT(), { opId, origin, name, message }))
  }
  versionCheckout(baseRev: bigint, opId: string, origin: string, commit: string) {
    return this.call<R.SaveResultT>(RequestPayload.VersionCheckoutRequest,
      Object.assign(new R.VersionCheckoutRequestT(), { baseRev, opId, origin, commit }))
  }

}

export const cardPath = (segments: string[]) => new D.CardPathT([...segments])
export const segs = (p: D.CardPathT | null | undefined): string[] => (p?.segments ?? []).map((x) => s(x))

/** DocumentChange builders. */
export function concat(parts: Uint8Array[]): Uint8Array {
  const n = parts.reduce((a, p) => a + p.length, 0)
  const out = new Uint8Array(n)
  let o = 0
  for (const p of parts) {
    out.set(p, o)
    o += p.length
  }
  return out
}

export async function sha256Hex(data: Uint8Array): Promise<string> {
  const d = await crypto.subtle.digest('SHA-256', data as unknown as ArrayBuffer)
  return [...new Uint8Array(d)].map((x) => x.toString(16).padStart(2, '0')).join('')
}

interface NativeClientError { code: number; message: string }
interface WasmClient {
  start(callback: (error: NativeClientError | null) => void): void
  call(frame: Uint8Array, timeoutMs: number, callback: (error: NativeClientError | null, frame: Uint8Array) => void): void
  onEvent(callback: (frame: Uint8Array) => void): void
  onClosed(callback: (error: NativeClientError) => void): void
  close(): void
  delete(): void
}
interface WasmModule { RpcClient: new (endpoint: string, protocol: number) => WasmClient }
let wasmModule: Promise<WasmModule> | undefined

const nativeError = (error: NativeClientError) => new RPCError(
  error.code === 2 ? 'timeout' : error.code === 3 ? 'protocol' : error.code === 5 || error.code === 6 ? 'unexpected' : 'disconnected',
  error.message,
)

/** Browser RPC connections use the shared C++ byte transport. */
export async function webSocketTransport(url: string, signal?: AbortSignal, protocol = 0): Promise<Transport> {
  if (!wasmModule) {
    const moduleUrl = '/client/tsugumi-client.js'
    wasmModule = import(moduleUrl).then((module) => module.default() as Promise<WasmModule>)
    wasmModule.catch(() => { wasmModule = undefined })
  }
  const module = await wasmModule
  signal?.throwIfAborted()
  const client = new module.RpcClient(url, protocol)
  let released = false
  const release = () => {
    if (released) return
    released = true
    client.close()
    client.delete()
  }
  const transport: Transport = {
    call: (frame, timeoutMs) => new Promise((resolve, reject) => {
      if (released) return reject(disconnected())
      client.call(frame, timeoutMs, (error, reply) => error ? reject(nativeError(error)) : resolve(reply))
    }),
    close: release,
  }
  client.onEvent((frame) => transport.onEvent?.(frame))
  client.onClosed((error) => {
    transport.onClose?.(error.message)
    // Free the binding after the C++ callback has unwound.
    queueMicrotask(release)
  })
  signal?.addEventListener('abort', release, { once: true })
  try {
    await new Promise<void>((resolve, reject) => client.start((error) => error ? reject(nativeError(error)) : resolve()))
    return transport
  } catch (error) {
    release()
    throw error
  } finally {
    signal?.removeEventListener('abort', release)
  }
}

/** Presentation choice made by the caller; core only reserves the id. */
export function cardStem(name: string): string {
  return name.replace(/[A-Z]/g, c => c.toLowerCase()).replace(/[^a-z0-9]+/g, '-').replace(/^-/, '').slice(0, 123).replace(/-$/, '') || 'card'
}
