import FlatBuffers
import Foundation

/// Typed wrappers for the core requests. Every mutating call takes an op_id
/// chosen by the caller so it can be re-sent or checked with OperationGet.
public extension RPCClient {
  func open(viewId: UUID?) async throws -> Tsugumi_RPC_OpenResultT {
    let r = Tsugumi_RPC_ProjectOpenRequestT()
    r.viewId = viewId?.uuidString.lowercased()
    return try await call(.projectopenrequest, r)
  }

  func closeSubscription(_ id: String) async throws {
    let r = Tsugumi_RPC_ProjectCloseRequestT()
    r.subscriptionId = id
    let _: Tsugumi_RPC_AckT = try await call(.projectcloserequest, r)
  }

  func inspect() async throws -> Tsugumi_RPC_ProjectSnapshotT {
    try await call(.projectinspectrequest, Tsugumi_RPC_ProjectInspectRequestT())
  }

  func save(baseRev: UInt64, opId: UUID, origin: String, changes: [Tsugumi_RPC_DocumentChangeT],
            renamed: [(from: [String], to: [String])] = []) async throws -> Tsugumi_RPC_SaveResultT {
    let r = Tsugumi_RPC_ProjectSaveRequestT()
    r.baseRev = baseRev
    r.opId = opId.uuidString.lowercased()
    r.origin = origin
    r.changes = changes
    r.renamedCards = renamed.map { x in
      let c = Tsugumi_RPC_CardRenameT()
      c.from = cardPath(x.from)
      c.to = cardPath(x.to)
      return c
    }
    return try await call(.projectsaverequest, r)
  }

  func reload(baseRev: UInt64, opId: UUID, origin: String) async throws -> Tsugumi_RPC_SaveResultT {
    let r = Tsugumi_RPC_ProjectReloadRequestT()
    r.baseRev = baseRev
    r.opId = opId.uuidString.lowercased()
    r.origin = origin
    return try await call(.projectreloadrequest, r)
  }

  func validate(atRev: UInt64, targets: [[String]] = [], executable: Bool = true) async throws -> Tsugumi_RPC_ProjectValidateResultT {
    let r = Tsugumi_RPC_ProjectValidateRequestT()
    r.atRev = atRev
    r.targets = targets.map { cardPath($0) }
    r.mode = executable ? .executable : .structure
    return try await call(.projectvalidaterequest, r)
  }

  func operation(_ opId: UUID) async throws -> Tsugumi_RPC_OperationResultT {
    let r = Tsugumi_RPC_OperationGetRequestT()
    r.opId = opId.uuidString.lowercased()
    return try await call(.operationgetrequest, r)
  }

  func fileList(path: String, atRev: UInt64, depth: UInt32 = 8, directories: Bool = false, cursor: String? = nil) async throws -> Tsugumi_RPC_SourcePageT {
    let r = Tsugumi_RPC_ProjectFileListRequestT()
    r.path = path
    r.atRev = atRev
    r.depth = depth
    r.includeDirectories = directories
    r.cursor = cursor
    r.limit = 500
    return try await call(.projectfilelistrequest, r)
  }

  /// Reads a whole file in chunks, verifying the final hash.
  func readFile(path: String, atRev: UInt64) async throws -> Data {
    var out = Data()
    var offset: UInt64 = 0
    var hash = ""
    while true {
      let r = Tsugumi_RPC_ProjectFileReadRequestT()
      r.path = path
      r.atRev = atRev
      r.offset = offset
      r.limitBytes = 1 << 20
      let c: Tsugumi_RPC_FileContentT = try await call(.projectfilereadrequest, r)
      out.append(contentsOf: c.bytes)
      hash = c.hash
      offset = c.nextOffset ?? offset
      if c.eof == true { break }
    }
    guard sha256Hex(out) == hash else { throw RPCFailure(kind: .integrityError, diagnostic: "hash mismatch for \(path)") }
    return out
  }

  func run(baseRev: UInt64, opId: UUID, card: [String], propagate: Bool, maxConcurrency: UInt32 = 4) async throws -> Tsugumi_RPC_RunAcceptedT {
    let r = Tsugumi_RPC_RunRequestT()
    r.baseRev = baseRev
    r.opId = opId.uuidString.lowercased()
    r.origin = "ui"
    r.card = cardPath(card)
    r.propagate = propagate
    r.maxConcurrency = maxConcurrency
    return try await call(.runrequest, r)
  }

  func cancel(runId: String) async throws -> Tsugumi_RPC_RunStatusT {
    let r = Tsugumi_RPC_RunCancelRequestT()
    r.runId = runId
    return try await call(.runcancelrequest, r)
  }

  func runStatus(runId: String? = nil) async throws -> Tsugumi_RPC_RunStatusT {
    let r = Tsugumi_RPC_RunStatusRequestT()
    r.runId = runId
    return try await call(.runstatusrequest, r)
  }

  func history(card: [String]? = nil, artifactsOnly: Bool = false, cursor: String? = nil, limit: UInt32 = 50) async throws -> Tsugumi_RPC_RunPageT {
    let r = Tsugumi_RPC_RunHistoryRequestT()
    r.card = card.map { cardPath($0) }
    r.artifactsOnly = artifactsOnly
    r.cursor = cursor
    r.limit = limit
    return try await call(.runhistoryrequest, r)
  }

  func invocations(runId: String, card: [String], cursor: String? = nil) async throws -> Tsugumi_RPC_InvocationPageT {
    let r = Tsugumi_RPC_RunInvocationsRequestT()
    r.runId = runId
    r.card = cardPath(card)
    r.cursor = cursor
    r.limit = 500
    return try await call(.runinvocationsrequest, r)
  }

  func record(runId: String, card: [String], invocation: UInt64? = nil, followLatest: Bool = false) async throws -> Tsugumi_RPC_RecordResultT {
    let r = Tsugumi_RPC_RunRecordRequestT()
    r.runId = runId
    r.card = cardPath(card)
    r.invocation = invocation
    r.followLatest = followLatest
    return try await call(.runrecordrequest, r)
  }

  func logRange(runId: String, card: [String]?, invocation: UInt64?, stream: Tsugumi_Data_LogStream,
                offset: UInt64 = 0, tail: Bool = false, limit: UInt32 = 65536) async throws -> Tsugumi_RPC_LogSliceT {
    let r = Tsugumi_RPC_LogRangeRequestT()
    r.runId = runId
    r.card = card.map { cardPath($0) }
    r.invocation = invocation
    r.stream = stream
    r.offset = tail ? nil : offset
    r.tail = tail
    r.limitBytes = limit
    return try await call(.lograngerequest, r)
  }

  func artifacts(runId: String, card: [String]? = nil) async throws -> [Tsugumi_RPC_ArtifactInfoT] {
    var out: [Tsugumi_RPC_ArtifactInfoT] = []
    var cursor: String?
    repeat {
      let r = Tsugumi_RPC_ArtifactListRequestT()
      r.runId = runId
      r.card = card.map { cardPath($0) }
      r.cursor = cursor
      r.limit = 500
      let page: Tsugumi_RPC_ArtifactPageT = try await call(.artifactlistrequest, r)
      out += page.artifacts.compactMap { $0 }
      cursor = page.nextCursor
    } while cursor != nil
    return out
  }

  func readArtifact(id: String, member: String?) async throws -> Data {
    var out = Data()
    var offset: UInt64 = 0
    var hash = ""
    while true {
      let r = Tsugumi_RPC_ArtifactReadRequestT()
      r.artifactId = id
      r.member = member
      r.offset = offset
      let c: Tsugumi_RPC_FileContentT = try await call(.artifactreadrequest, r)
      out.append(contentsOf: c.bytes)
      hash = c.hash
      offset = c.nextOffset ?? offset
      if c.eof == true { break }
    }
    guard sha256Hex(out) == hash else { throw RPCFailure(kind: .integrityError, diagnostic: "artifact hash mismatch") }
    return out
  }

  func setVisible(subscription: String, cards: [[String]], enabled: Bool = true) async throws {
    let r = Tsugumi_RPC_ViewSetVisibleRequestT()
    r.subscriptionId = subscription
    r.cards = cards.map { cardPath($0) }
    r.enabled = enabled
    let _: Tsugumi_RPC_AckT = try await call(.viewsetvisiblerequest, r)
  }

  func saveView(viewId: UUID, baseRev: UInt64, view: Tsugumi_RPC_ViewStateT) async throws -> Tsugumi_RPC_ViewSaveResultT {
    let r = Tsugumi_RPC_ViewSaveRequestT()
    r.viewId = viewId.uuidString.lowercased()
    r.baseViewRev = baseRev
    r.view = view
    return try await call(.viewsaverequest, r)
  }

  /// Copies definitions (and their sources) from a granted project snapshot.
  func importDefinitions(baseRev: UInt64, opId: UUID, origin: String, grant: String, definitions: [String]) async throws -> Tsugumi_RPC_ProjectImportResultT {
    let r = Tsugumi_RPC_ProjectImportRequestT()
    r.baseRev = baseRev
    r.opId = opId.uuidString.lowercased()
    r.origin = origin
    r.sourceGrant = grant
    r.definitions = definitions
    return try await call(.projectimportrequest, r)
  }

  /// Generates the Card's contracts (_inputs.fbs, _outputs.fbs, _params.fbs)
  /// for one revision and returns the file list when ready.
  func generatedContracts(card: [String], atRev: UInt64) async throws -> [Tsugumi_RPC_SDKFileT] {
    let r = Tsugumi_RPC_CardSDKPrepareRequestT()
    r.card = cardPath(card)
    r.atRev = atRev
    r.opId = UUID().uuidString.lowercased()
    var res: Tsugumi_RPC_CardSDKPrepareResultT = try await call(.cardsdkpreparerequest, r)
    for _ in 0..<100 where res.state == .pending {
      try await Task.sleep(for: .milliseconds(100))
      let s = Tsugumi_RPC_CardSDKStatusRequestT()
      s.opId = res.opId
      res = try await call(.cardsdkstatusrequest, s)
    }
    return res.files.compactMap { $0 }
  }

  /// Deletes the project's build caches; documents and history are kept.
  func clearCache(opId: UUID) async throws -> Tsugumi_RPC_ProjectCacheClearResultT {
    let r = Tsugumi_RPC_ProjectCacheClearRequestT()
    r.opId = opId.uuidString.lowercased()
    return try await call(.projectcacheclearrequest, r, timeout: .seconds(300))
  }

  /// Deletes finished runs (all, or those that ended before the date) with
  /// their files, in the VM and in the project's copy.
  func deleteHistory(opId: UUID, endedBefore: Date?) async throws -> Tsugumi_RPC_RunHistoryDeleteResultT {
    let r = Tsugumi_RPC_RunHistoryDeleteRequestT()
    r.opId = opId.uuidString.lowercased()
    r.endedBefore = endedBefore.map { Int64($0.timeIntervalSince1970 * 1000) }
    return try await call(.runhistorydeleterequest, r, timeout: .seconds(300))
  }

  /// The types of an _outputs.fbs text as the core compiles them (bfbs); with
  /// a port and a type, also the text with that port changed. Saves nothing.
  func outputTypes(text: String, port: String? = nil, typeName: String? = nil,
                   kind: Tsugumi_Data_PortKind = .flatbuffer) async throws -> Tsugumi_RPC_OutputTypesResultT {
    let r = Tsugumi_RPC_OutputTypesRequestT()
    r.text = text
    r.port = port
    r.typeName = typeName
    if port != nil { r.kind = kind }
    return try await call(.outputtypesrequest, r)
  }

  func format(path: String, text: String) async throws -> Tsugumi_RPC_ProjectFileFormatResultT {
    let r = Tsugumi_RPC_ProjectFileFormatRequestT()
    r.path = path
    r.text = text
    return try await call(.projectfileformatrequest, r)
  }

  func runPlan(atRev: UInt64, card: [String], propagate: Bool) async throws -> Tsugumi_RPC_RunPlanResultT {
    let r = Tsugumi_RPC_RunPlanRequestT()
    r.atRev = atRev
    r.card = cardPath(card)
    r.propagate = propagate
    return try await call(.runplanrequest, r)
  }
}

/// DocumentChange builders.
public enum Change {
  public static func setProject(_ json: String) -> Tsugumi_RPC_DocumentChangeT {
    let v = Tsugumi_RPC_SetProjectT()
    v.json = json
    return wrap(v, .setproject)
  }

  public static func setLayout(_ json: String) -> Tsugumi_RPC_DocumentChangeT {
    let v = Tsugumi_RPC_SetLayoutT()
    v.json = json
    return wrap(v, .setlayout)
  }

  public static func putDefinition(id: String, cardJSON: String, metaJSON: String?, linkJSON: String? = nil) -> Tsugumi_RPC_DocumentChangeT {
    let v = Tsugumi_RPC_PutDefinitionT()
    v.id = id
    v.cardJson = linkJSON == nil ? cardJSON : ""
    v.metaJson = metaJSON
    v.linkJson = linkJSON
    return wrap(v, .putdefinition)
  }

  public static func renameDefinition(id: String, to: String) -> Tsugumi_RPC_DocumentChangeT {
    let v = Tsugumi_RPC_RenameDefinitionT()
    v.id = id
    v.to = to
    return wrap(v, .renamedefinition)
  }

  public static func removeDefinition(id: String) -> Tsugumi_RPC_DocumentChangeT {
    let v = Tsugumi_RPC_RemoveDefinitionT()
    v.id = id
    return wrap(v, .removedefinition)
  }

  public static func putSource(path: String, bytes: Data, baseHash: String?) -> Tsugumi_RPC_DocumentChangeT {
    let v = Tsugumi_RPC_PutSourceT()
    v.path = path
    v.bytes = [UInt8](bytes)
    v.baseHash = baseHash
    return wrap(v, .putsource)
  }

  public static func removeSource(path: String, baseHash: String) -> Tsugumi_RPC_DocumentChangeT {
    let v = Tsugumi_RPC_RemoveSourceT()
    v.path = path
    v.baseHash = baseHash
    return wrap(v, .removesource)
  }

  private static func wrap(_ v: NativeObject, _ t: Tsugumi_RPC_DocumentChangeValue) -> Tsugumi_RPC_DocumentChangeT {
    let c = Tsugumi_RPC_DocumentChangeT()
    c.value = Tsugumi_RPC_DocumentChangeValueUnion(v, type: t)
    return c
  }
}
