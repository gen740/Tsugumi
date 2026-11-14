// Typed project operations. The shared C++ client owns transport, validation,
// request correlation, deadlines and disconnect handling (Wasm in the browser).
import { Builder, ByteBuffer, Encoding } from 'flatbuffers'
import * as D from '../gen/tsugumi/data'
import * as R from '../gen/tsugumi/rpc'
import { Message, MessageT } from '../gen/tsugumi/rpc/message'
import { Payload } from '../gen/tsugumi/rpc/payload'
import { Reply } from '../gen/tsugumi/rpc/reply'
import { ReplyT } from '../gen/tsugumi/rpc/reply'
import { RequestPayload } from '../gen/tsugumi/rpc/request-payload'
import { RequestT } from '../gen/tsugumi/rpc/request'
import { NotificationT } from '../gen/tsugumi/rpc/notification'
import { L } from '../i18n'
import type { LayoutBar, LayoutBox, LayoutPort, LayoutProposal, LayoutScene } from '../model/layoutScene'

export { D, R, RequestPayload }

/** Strings of the object API are always decoded text. */
export const s = (v: string | Uint8Array | null | undefined): string => (typeof v === 'string' ? v : v ? new TextDecoder().decode(v) : '')
export const sOpt = (v: string | Uint8Array | null | undefined): string | undefined => (v === null || v === undefined ? undefined : s(v))

/** Preserve invalid display text as an unavailable display, before lossy decoding. */
export function unpackDisplay(wire: D.Display): D.DisplayT {
  let bytes = 0
  const decode = (v: string | Uint8Array | null) => {
    if (v == null) throw new Error('missing display text')
    if (v instanceof Uint8Array) {
      bytes += v.length + 8
      if (bytes > 16 << 20) throw new Error('display content exceeds 16 MiB')
      new TextDecoder('utf-8', { fatal: true }).decode(v)
    }
  }
  try {
    switch (wire.valueType()) {
      case D.DisplayValue.DisplayText: decode((wire.value(new D.DisplayText()) as D.DisplayText).text(Encoding.UTF8_BYTES)); break
      case D.DisplayValue.DisplayScalar: decode((wire.value(new D.DisplayScalar()) as D.DisplayScalar).label(Encoding.UTF8_BYTES)); break
      case D.DisplayValue.DisplayTable: {
        const t = wire.value(new D.DisplayTable()) as D.DisplayTable
        if (!t.columnsLength() || t.columnsLength() > 64 || t.rowsLength() > 10000) throw new Error('invalid table size')
        for (let i=0;i<t.columnsLength();i++) decode(t.columns(i, Encoding.UTF8_BYTES))
        for (let i=0;i<t.rowsLength();i++) {
          const row=t.rows(i)!
          if (row.cellsLength() !== t.columnsLength()) throw new Error('invalid table row')
          for (let j=0;j<row.cellsLength();j++) decode(row.cells(j, Encoding.UTF8_BYTES))
        }
        break
      }
      case D.DisplayValue.DisplayImage: {
        const im=wire.value(new D.DisplayImage()) as D.DisplayImage
        if (im.dataLength() > 8 << 20) throw new Error('invalid image size')
        decode(im.mediaType(Encoding.UTF8_BYTES))
        const alt=im.alt(Encoding.UTF8_BYTES); if (alt != null) decode(alt)
        break
      }
      case D.DisplayValue.DisplayPointCloud: {
        const p=wire.value(new D.DisplayPointCloud()) as D.DisplayPointCloud
        if (p.xLength() > 1000000 || p.yLength() !== p.xLength() || p.zLength() !== p.xLength() ||
            (p.colorsLength() && p.colorsLength() !== p.xLength())) throw new Error('invalid point cloud size')
        break
      }
      case D.DisplayValue.DisplayModule: {
        const m=wire.value(new D.DisplayModule()) as D.DisplayModule
        if (m.blobsLength() > 64) throw new Error('invalid module size')
        let blobBytes = 0
        for (let i=0;i<m.blobsLength();i++) blobBytes += m.blobs(i)!.dataLength()
        if (blobBytes > 8 << 20) throw new Error('invalid module size')
        decode(m.module(Encoding.UTF8_BYTES)); decode(m.configJson(Encoding.UTF8_BYTES))
        for(let i=0;i<m.blobsLength();i++) decode(m.blobs(i)!.name(Encoding.UTF8_BYTES))
        break
      }
    }
    return wire.unpack()
  } catch {
    return Object.assign(new D.DisplayT(), { card: wire.card()?.unpack(), runId: wire.runId(),
      invocation: wire.invocation(), revision: wire.revision() })
  }
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
  call(frame: Uint8Array, timeoutMs: number): Promise<Uint8Array>
  close(): void
  /** Set by the client. */
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
  async connect(token: string | undefined, language: 'ja' | 'en', clientId = uuid(), expectedProjectId?: string): Promise<R.HelloResultT> {
    const req = Object.assign(new R.SessionHelloRequestT(), { protocol: 1, clientId, token: token ?? null, expectedProjectId: expectedProjectId ?? null, language })
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
      if (message.payloadType() !== Payload.Notification) throw new Error('unexpected notification')
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

  private async dispatch(frame: Uint8Array, id: bigint, raw: boolean, timeoutMs: number): Promise<ReplyT | Uint8Array> {
    if (this._closed) throw disconnected()
    const bytes = await this.transport.call(frame, timeoutMs)
    // The native/Wasm library has already verified the entire FlatBuffer and ID.
    const message = Message.getRootAsMessage(new ByteBuffer(bytes))
    if (message.requestId() !== id || message.payloadType() !== Payload.Reply) {
      this.close()
      throw new RPCError('protocol', 'unexpected RPC reply')
    }
    if (raw) return bytes
    const wire = message.payload(new Reply()) as Reply
    const reply = wire.valueType() === R.ReplyPayload.RecordResult
      ? new ReplyT(R.ReplyPayload.RecordResult, unpackRecordResult(wire.value(new R.RecordResult()) as R.RecordResult))
      : wire.unpack()
    return reply
  }

  /** Sends one request and waits for its reply. Failure replies throw RPCFailure. */
  async call<T>(type: RequestPayload, value: RequestT['value'], timeoutMs = 60_000): Promise<T> {
    const id = this.nextId++
    const msg = new MessageT(id, Payload.Request, new RequestT(type, value))
    const b = new Builder(1024)
    b.finish(msg.pack(b), 'TSG1')
    const reply = (await this.dispatch(b.asUint8Array().slice(), id, false, timeoutMs)) as ReplyT
    if (reply.valueType === R.ReplyPayload.Failure) throw new RPCFailure(reply.value as D.FailureT)
    if (reply.valueType === R.ReplyPayload.NONE) throw new RPCError('unexpected', 'empty reply')
    return reply.value as T
  }

  /** Sends a request whose table is written by `build` (schema-driven tools).
   *  Returns the reply union type and the reply table position. */
  async callRaw(payloadType: number, build: (b: Builder) => number, timeoutMs = 60_000): Promise<{ type: number; bb: ByteBuffer; table: number }> {
    const id = this.nextId++
    const b = new Builder(4096)
    const value = build(b)
    R.Request.startRequest(b)
    R.Request.addValueType(b, payloadType)
    R.Request.addValue(b, value)
    const req = R.Request.endRequest(b)
    const msg = Message.createMessage(b, id, Payload.Request, req)
    b.finish(msg, 'TSG1')
    const frame = (await this.dispatch(b.asUint8Array().slice(), id, true, timeoutMs)) as Uint8Array
    const bb = new ByteBuffer(frame)
    const m = Message.getRootAsMessage(bb)
    const reply = m.payload(new Reply()) as Reply
    const t = reply.value(new R.Ack()) as R.Ack | null
    if (!t) throw new RPCError('unexpected', 'no reply value')
    return { type: reply.valueType(), bb, table: t.bb_pos }
  }

  cardId(name: string, projectPath = '') {
    return this.call<R.CardIdResultT>(RequestPayload.CardIdRequest, Object.assign(new R.CardIdRequestT(), { name, projectPath }))
  }

  // ---- Typed wrappers ---------------------------------------------------------

  open(viewId: string | null) {
    return this.call<R.OpenResultT>(RequestPayload.ProjectOpenRequest, Object.assign(new R.ProjectOpenRequestT(), { viewId }))
  }
  closeSubscription(id: string) {
    return this.call<R.AckT>(RequestPayload.ProjectCloseRequest, Object.assign(new R.ProjectCloseRequestT(), { subscriptionId: id }))
  }
  inspect() {
    return this.call<R.ProjectSnapshotT>(RequestPayload.ProjectInspectRequest, new R.ProjectInspectRequestT())
  }
  save(baseRev: bigint, opId: string, origin: string, changes: R.DocumentChangeT[]) {
    return this.call<R.SaveResultT>(RequestPayload.ProjectSaveRequest, Object.assign(new R.ProjectSaveRequestT(), { baseRev, opId, origin, changes }))
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
      Object.assign(new R.RunHistoryDeleteRequestT(), { opId, endedBefore: endedBefore === null ? null : BigInt(endedBefore) }), 300_000)
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
  reload(baseRev: bigint, opId: string, origin: string, approvedPermissions: string[] = []) {
    return this.call<R.SaveResultT>(RequestPayload.ProjectReloadRequest, Object.assign(new R.ProjectReloadRequestT(), { baseRev, opId, origin, approvedPermissions }))
  }
  operation(opId: string) {
    return this.call<R.OperationResultT>(RequestPayload.OperationGetRequest, Object.assign(new R.OperationGetRequestT(), { opId }))
  }

  /** Reads a whole file in chunks, verifying the final hash. */
  async readFile(path: string, atRev: bigint): Promise<Uint8Array> {
    const parts: Uint8Array[] = []
    let offset = 0n
    let hash = ''
    for (;;) {
      const r = Object.assign(new R.ProjectFileReadRequestT(), { path, atRev, offset, limitBytes: 1 << 20 })
      const c = await this.call<R.FileContentT>(RequestPayload.ProjectFileReadRequest, r)
      parts.push(Uint8Array.from(c.bytes))
      hash = s(c.hash)
      offset = c.nextOffset ?? offset
      if (c.eof) break
    }
    const out = concat(parts)
    if ((await sha256Hex(out)) !== hash) throw new RPCFailure({ kind: D.FailureKind.integrity_error, diagnostic: `hash mismatch for ${path}` })
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
  logRange(runId: string, card: string[] | null, invocation: bigint | null, stream: D.LogStream, offset = 0n, tail = false, limit = 65536) {
    return this.call<R.LogSliceT>(
      RequestPayload.LogRangeRequest,
      Object.assign(new R.LogRangeRequestT(), {
        runId, card: card ? cardPath(card) : null, invocation, stream, offset: tail ? null : offset, tail, limitBytes: limit,
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
    let hash = ''
    for (;;) {
      const r = Object.assign(new R.ArtifactReadRequestT(), { artifactId, member, offset })
      const c = await this.call<R.FileContentT>(RequestPayload.ArtifactReadRequest, r)
      parts.push(Uint8Array.from(c.bytes))
      hash = s(c.hash)
      offset = c.nextOffset ?? offset
      if (c.eof) break
    }
    const out = concat(parts)
    if ((await sha256Hex(out)) !== hash) throw new RPCFailure({ kind: D.FailureKind.integrity_error, diagnostic: 'artifact hash mismatch' })
    return out
  }
  setVisible(subscriptionId: string, cards: string[][], enabled = true) {
    return this.call<R.AckT>(
      RequestPayload.ViewSetVisibleRequest,
      Object.assign(new R.ViewSetVisibleRequestT(), { subscriptionId, cards: cards.map(cardPath), enabled }),
    )
  }
  saveView(viewId: string, baseViewRev: bigint, view: R.ViewStateT) {
    return this.call<R.ViewSaveResultT>(RequestPayload.ViewSaveRequest, Object.assign(new R.ViewSaveRequestT(), { viewId, baseViewRev, view }))
  }
  /** Each Card's latest invocation of a run. */
  latestInvocations(runId: string) {
    return this.call<R.InvocationPageT>(RequestPayload.RunLatestInvocationsRequest, Object.assign(new R.RunLatestInvocationsRequestT(), { runId }))
  }
  /** Looks a flake up for nixSources (nothing is saved). */
  resolveNixSource(ref: string) {
    return this.call<R.NixSourceResolvedT>(RequestPayload.NixSourceResolveRequest, Object.assign(new R.NixSourceResolveRequestT(), { ref }), 300_000)
  }
  /** Where tsugumid would put a scope's Cards, frames and notes, and the port orders that go with it (nothing is saved). */
  async layoutArrange(scene: LayoutScene): Promise<LayoutProposal> {
    return proposalOf(await this.call<R.LayoutProposalT>(RequestPayload.LayoutArrangeRequest, new R.LayoutArrangeRequestT(sceneRequest(scene))))
  }
  /** The port orders that cross fewer wires with every Card where it is; `around`: only near the Cards just moved. */
  async layoutUntangle(scene: LayoutScene, around: string[]): Promise<LayoutProposal> {
    return proposalOf(await this.call<R.LayoutProposalT>(RequestPayload.LayoutUntangleRequest, new R.LayoutUntangleRequestT(sceneRequest(scene), around)))
  }
  /** Deletes the project's build caches; documents and history are kept. */
  clearCache(opId: string) {
    return this.call<R.ProjectCacheClearResultT>(RequestPayload.ProjectCacheClearRequest, Object.assign(new R.ProjectCacheClearRequestT(), { opId }), 300_000)
  }
  deleteRunScratch(opId: string, paths: string[]) {
    return this.call<R.RunScratchDeleteResultT>(RequestPayload.RunScratchDeleteRequest, new R.RunScratchDeleteRequestT(opId, paths), 300_000)
  }
  cacheUsage() {
    return this.call<R.ProjectCacheUsageResultT>(RequestPayload.ProjectCacheUsageRequest, new R.ProjectCacheUsageRequestT())
  }
  // ---- Versions (git): core/src/versions.cc
  versionGraph(limit = 500) {
    return this.call<R.VersionGraphT>(RequestPayload.VersionGraphRequest, Object.assign(new R.VersionGraphRequestT(), { limit }))
  }
  versionCanvas(commit: string) {
    return this.call<R.VersionCanvasT>(RequestPayload.VersionCanvasRequest, Object.assign(new R.VersionCanvasRequestT(), { commit }))
  }
  versionDiff(commit: string, fromCommit: string | null = null) {
    return this.call<R.VersionDiffT>(RequestPayload.VersionDiffRequest, Object.assign(new R.VersionDiffRequestT(), { commit, fromCommit }))
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
  format(path: string, text: string) {
    return this.call<R.ProjectFileFormatResultT>(RequestPayload.ProjectFileFormatRequest, Object.assign(new R.ProjectFileFormatRequestT(), { path, text }))
  }
}

function sceneRequest(scene: LayoutScene): R.LayoutSceneT {
  const ports = (list: LayoutPort[]) => list.map((p) => new R.LayoutPortT(p.name, p.x, p.y))
  const boxes = (list: LayoutBox[]) => list.map((b) => new R.LayoutBoxT(b.id, b.x, b.y, b.width, b.height))
  const bar = (b: LayoutBar | undefined) => b && new R.LayoutBarT(b.ends.map((e) => new R.LayoutBarEndT(e.name, e.card, e.port)), b.at?.x ?? null, b.at?.y ?? null)
  return new R.LayoutSceneT(
    scene.flow, scene.grid, scene.snap,
    scene.cards.map((c) => new R.LayoutCardT(c.id, c.x, c.y, c.width, c.height, ports(c.inputs), ports(c.outputs), c.reorderable)),
    scene.wires.map((w) => new R.LayoutWireT(w.from, w.fromPort, w.to, w.toPort)),
    boxes(scene.frames), boxes(scene.notes), bar(scene.inputs) ?? null, bar(scene.outputs) ?? null,
  )
}

function proposalOf(reply: R.LayoutProposalT): LayoutProposal {
  const places = (list: R.LayoutPlaceT[]) => list.map((p) => ({ id: s(p.id), x: p.x, y: p.y }))
  return {
    cards: places(reply.cards), frames: places(reply.frames), notes: places(reply.notes),
    portOrders: reply.portOrders.map((o) => ({ id: s(o.id), inputs: o.inputs.map((n) => s(n)), outputs: o.outputs.map((n) => s(n)) })),
  }
}

export const cardPath = (segments: string[]) => new D.CardPathT([...segments])
export const segs = (p: D.CardPathT | null | undefined): string[] => (p?.segments ?? []).map((x) => s(x))

/** DocumentChange builders. */
export const Change = {
  setProject: (json: string) => new R.DocumentChangeT(R.DocumentChangeValue.SetProject, new R.SetProjectT(json)),
  setLayout: (json: string) => new R.DocumentChangeT(R.DocumentChangeValue.SetLayout, new R.SetLayoutT(json)),
  putDefinition: (id: string, cardJson: string, metaJson: string | null, linkJson: string | null = null) =>
    new R.DocumentChangeT(R.DocumentChangeValue.PutDefinition, new R.PutDefinitionT(id, linkJson === null ? cardJson : '', metaJson, linkJson)),
  removeDefinition: (id: string) => new R.DocumentChangeT(R.DocumentChangeValue.RemoveDefinition, new R.RemoveDefinitionT(id)),
  putSource: (path: string, bytes: Uint8Array, baseHash: string | null) =>
    new R.DocumentChangeT(R.DocumentChangeValue.PutSource, new R.PutSourceT(path, Array.from(bytes), baseHash)),
  removeSource: (path: string, baseHash: string) =>
    new R.DocumentChangeT(R.DocumentChangeValue.RemoveSource, new R.RemoveSourceT(path, baseHash)),
}

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

/** Browser project connections use the shared C++ byte transport. */
export async function webSocketTransport(url: string): Promise<Transport> {
  if (!wasmModule) {
    const moduleUrl = '/client/tsugumi-client.js'
    wasmModule = import(moduleUrl).then((module) => module.default() as Promise<WasmModule>)
    wasmModule.catch(() => { wasmModule = undefined })
  }
  const module = await wasmModule
  const client = new module.RpcClient(url, 0)
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
  try {
    await new Promise<void>((resolve, reject) => client.start((error) => error ? reject(nativeError(error)) : resolve()))
    return transport
  } catch (error) {
    release()
    throw error
  }
}
