import TsugumiAgentClient
import FlatBuffers
import Foundation
import Observation
import TsugumiLauncher
import TsugumiRPC

typealias CardKey = String  // "a/b"

func key(_ path: [String]) -> CardKey { path.joined(separator: "/") }
func path(_ key: CardKey) -> [String] { key.isEmpty ? [] : key.split(separator: "/").map(String.init) }

struct Diagnostic: Identifiable, Hashable {
  let id = UUID()
  let error: Bool
  let code: String
  let message: String
  let card: [String]?
  let path: String?
  let offset: UInt64?

  init(_ d: Tsugumi_Data_DiagnosticT) {
    error = d.severity != .warning
    code = d.code
    message = d.message
    card = d.card?.path
    path = d.path
    offset = d.offset
  }
}

struct PortInfo: Hashable {
  let name: String
  let kind: Tsugumi_Data_PortKind
  let optional: Bool
  let rootType: String?
  var executionShape: Tsugumi_Data_ExecutionShape = .unspecified
}

struct ResolvedCard {
  let path: [String]
  let inputs: [PortInfo]
  let outputs: [PortInfo]
  let diagnostics: [Diagnostic]
  var nixReferences: [CardNixReference] = []
}

/// Decode a retained display without changing the live canvas.
func recordedDisplay(_ d: Tsugumi_Data_DisplayT) -> DisplaySnapshot? {
  let value: DisplayValue
  switch d.value?.type {
  case .displaytext:
    let t = d.value?.value as? Tsugumi_Data_DisplayTextT
    value = t?.markdown == true ? .markdown(t?.text ?? "") : .text(t?.text ?? "")
  case .displayscalar:
    let s = d.value?.value as? Tsugumi_Data_DisplayScalarT
    value = .scalar(s?.value ?? .nan, s?.label ?? "")
  case .displaytable:
    let t = d.value?.value as? Tsugumi_Data_DisplayTableT
    value = .table(columns: (t?.columns ?? []).compactMap { $0 }, rows: (t?.rows ?? []).compactMap { $0?.cells.compactMap { $0 } })
  case .displayimage:
    let im = d.value?.value as? Tsugumi_Data_DisplayImageT
    value = .image(mediaType: im?.mediaType ?? "", data: Data(im?.data ?? []), alt: im?.alt ?? "")
  case .displaypointcloud:
    let p = d.value?.value as? Tsugumi_Data_DisplayPointCloudT
    value = .points(x: p?.x ?? [], y: p?.y ?? [], z: p?.z ?? [], colors: p?.colors ?? [])
  case .displaymodule:
    let m = d.value?.value as? Tsugumi_Data_DisplayModuleT
    let names = (m?.blobs ?? []).compactMap { $0?.name }
    guard Set(names).count == names.count else {
      return DisplaySnapshot(runId: d.runId, invocation: d.invocation ?? 0, revision: d.revision ?? 0, value: .unavailable)
    }
    value = .module(name: m?.module ?? "", json: m?.configJson ?? "", blobs: Dictionary(uniqueKeysWithValues: (m?.blobs ?? []).compactMap { $0 }.map { ($0.name, Data($0.data)) }))
  default: value = .unavailable
  }
  return DisplaySnapshot(runId: d.runId, invocation: d.invocation ?? 0, revision: d.revision ?? 0, value: value)
}

/// One stage of an invocation as the core recorded it (core clock, ms).
struct StageRecord: Equatable, Hashable {
  var stage: Stage
  var outcome: Tsugumi_Data_StageOutcome
  var start: Int64?
  var end: Int64?
}

/// Latest invocation of a Card in the current run, as the core reports it.
struct CardRunState: Equatable {
  var state: Tsugumi_Data_CardState
  /// Number of the latest invocation in this run (1, 2, …); 0 = not run.
  var invocation: UInt64
  var batchActivation: UInt64?
  var phase: Tsugumi_Data_Phase
  var failure: String?
  var executionState: Tsugumi_Data_RemoteExecutionState?
  var transfer: String?
  /// Stages of the latest invocation, in the order the core ran them.
  var stages: [StageRecord] = []
  var cached = false
  var workerReused = false
  /// Outputs the latest invocation sent, and every output that sent a value
  /// in this run (a branch lists only the side it chose).
  var sent: Set<String> = []
  var sentInRun: Set<String> = []
  var startedAt: Int64?
  var endedAt: Int64?
  var readyAt: Int64?

  var stage: Stage? { stages.last.map(\.stage) ?? (state == .waiting || state == .ready ? .waiting : nil) }
}

extension Tsugumi_Data_RemoteExecutionState {
  var label: String {
    switch self {
    case .queued: return String(localized: "実行サービスで待機中")
    case .running: return String(localized: "実行中")
    case .disconnected: return String(localized: "接続断（結果未確認）")
    case .cancelling: return String(localized: "取り消し確認中")
    case .succeeded: return String(localized: "成功")
    case .failed: return String(localized: "失敗")
    case .cancelled: return String(localized: "停止済み")
    case .interrupted: return String(localized: "中断")
    default: return String(localized: "不明")
    }
  }

  var pendingLabel: String? {
    switch self {
    case .queued, .disconnected, .cancelling: return label
    default: return nil
    }
  }
}

/// One invocation on the run timeline (times in ms since 1970, from the core).
extension TimelineEntry {
  static func stages(_ list: [Tsugumi_RPC_StageTimeT?]) -> [StageRecord] {
    list.compactMap { $0 }.compactMap { t in Stage(phase: t.phase).map { StageRecord(stage: $0, outcome: t.outcome, start: t.start, end: t.end) } }
  }
}

struct TimelineEntry: Identifiable, Hashable {
  var id: String { "\(card)#\(invocation)" }
  let card: CardKey
  let invocation: UInt64
  var batchActivation: UInt64?
  var state: Tsugumi_Data_CardState
  var startedAt: Int64?
  var endedAt: Int64?
  /// Stages of this invocation (core clock), for the timeline.
  var stages: [StageRecord] = []
}

/// Draft of one Card's definition (_card.json and optional _meta.json).
struct DefinitionDraft: Equatable {
  var card: JSONValue
  var meta: JSONValue?
}

/// A project inside this one (externals/<name>/, nested further): the inner
/// canvas of a Card in Card. A copy owned by this project, edited here like
/// the project's own documents (written back as its files).
struct ExternalProject: Equatable {
  var project: JSONValue
  var layout: JSONValue
  /// Card id -> its definition files (_card.json and optional _meta.json).
  var definitions: [String: DefinitionDraft]
  /// Removed (the folder goes with it).



}

/// Save/sync state, shown separately from the run state.
enum SyncState: Equatable {
  case saved
  case saving
  case failed(String)
  case offline
}

enum DisplayValue {
  case unavailable
  case text(String)
  /// Markdown with TeX math (the SDK's Markdown display).
  case markdown(String)
  case scalar(Double, String)
  case table(columns: [String], rows: [[String]])
  case image(mediaType: String, data: Data, alt: String)
  case points(x: [Float], y: [Float], z: [Float], colors: [UInt32])
  case module(name: String, json: String, blobs: [String: Data] = [:])
}

extension DisplayValue {
  var displayable: Bool {
    switch self {
    case .unavailable: return false
    case .text(let text), .markdown(let text): return text.utf8.count <= 1 << 20
    case .scalar(let value, _): return value.isFinite
    case .table(let columns, let rows):
      return !columns.isEmpty && columns.count <= 64 && rows.count <= 10000 && columns.allSatisfy { $0.utf8.count <= 4096 } &&
        rows.allSatisfy { $0.count == columns.count && $0.allSatisfy { $0.utf8.count <= 4096 } }
    case .image(let media, let data, _):
      return data.count <= 8 << 20 && (media == "image/png" ? data.count > 8 && data.starts(with: [137,80,78,71,13,10,26,10]) :
        media == "image/jpeg" && data.count > 3 && data.starts(with: [255,216]))
    case .points(let x, let y, let z, let colors):
      return x.count <= 1000000 && x.count == y.count && x.count == z.count && (colors.isEmpty || colors.count == x.count) &&
        [x,y,z].allSatisfy { $0.allSatisfy(\.isFinite) }
    case .module(let name, let json, let blobs):
      let pattern = #"^/nix/store/[0123456789abcdfghijklmnpqrsvwxyz]{32}-[a-zA-Z0-9+._?=-]+$"#
      return name.range(of: pattern, options: .regularExpression) != nil && json.utf8.count <= 1 << 20 &&
        (try? JSONSerialization.jsonObject(with: Data(json.utf8), options: [.fragmentsAllowed])) != nil &&
        blobs.count <= 64 && blobs.keys.allSatisfy { $0 != "." && $0 != ".." && $0.range(of: #"^[a-zA-Z0-9_.-]{1,128}$"#, options: .regularExpression) != nil } &&
        blobs.values.reduce(0) { $0 + $1.count } <= 8 << 20
    }
  }
}

struct DisplaySnapshot {
  struct Identity: Equatable {
    let runId: String
    let invocation: UInt64
    let revision: UInt64
  }
  var identity: Identity { Identity(runId: runId, invocation: invocation, revision: revision) }
  let runId: String
  let invocation: UInt64
  let revision: UInt64
  /// Preserve the result's content even when it cannot be rendered.
  let content: DisplayValue
  let value: DisplayValue
  /// What the Card's result row says of it. Worked out once, here: the canvas
  /// asks at every move, and a figure's title is found by parsing its JSON.
  let summary: String

  init(runId: String, invocation: UInt64, revision: UInt64, value: DisplayValue) {
    self.runId = runId
    self.invocation = invocation
    self.revision = revision
    self.content = value
    self.value = value.displayable ? value : .unavailable
    summary = Self.summary(of: self.value)
  }
}

/// One open project: shared by every window showing it. Owns the RPC
/// connection, the confirmed document (from tsugumid), the local draft with
/// undo/redo, run progress, displays and logs.
@MainActor
@Observable
final class ProjectSession {
  var projectTrust: Int?
  var trustedFolder = ""
  func refreshProjectTrust() async throws {
    try await UIScript.shared.registerTrustedFolderIfRequested()
    let result = try await CoreHostClient.shared.call("ProjectTrustRequest", ["path": identity.url.path])
    projectTrust = result["decision"] as? Int ?? 0
    trustedFolder = result["trusted_folder"] as? String ?? ""
  }
  func setProjectTrust(_ trusted: Bool) async {
    do {
      let result = try await CoreHostClient.shared.call("ProjectTrustRequest", ["path": identity.url.path, "decision": UInt8(trusted ? 1 : 2)])
      projectTrust = result["decision"] as? Int ?? 0
      trustedFolder = result["trusted_folder"] as? String ?? ""
    } catch { connectionMessage = String(describing: error) }
  }

  /// Derived layout remains a read-only host query.
  var prepareLayout: (String, String, [String: String]) async throws -> String = {
    try await CoreHostClient.shared.completeLayout(project: $0, layout: $1, definitions: $2)
  }
  var referenceIdProvider: ((String, String, [String]) async throws -> String)?
  var cardIdProvider: ((String, String) async throws -> String)?
  let identity: ProjectIdentity
  let environments: EnvironmentManager
  let viewId: UUID

  // Connection
  private(set) var environment: EnvironmentState = .stopped
  private(set) var connected = false
  /// The project's terminal tabs and their shells (Workbench/ProjectTerminal.swift).
  let terminals: ProjectTerminals
  /// Closed for good (close): it will not connect again.
  private(set) var closed = false
  private(set) var connectionMessage: String?
  @ObservationIgnored private var client: RPCClient?
  @ObservationIgnored private var subscription: String?
  @ObservationIgnored private var lastSeq: UInt64 = 0
  @ObservationIgnored private var listenTask: Task<Void, Never>?
  /// The one reconnection in progress (a gap and a lost connection reported
  /// together start it once).
  @ObservationIgnored private var reconnectTask: Task<Void, Never>?
  @ObservationIgnored private var attachmentTask: Task<[(folder: String, resource: String)], Error>?
  @ObservationIgnored private var attachmentID: UUID?
  @ObservationIgnored private var observerToken: UUID?
  private(set) var projectId: String?

  // Confirmed document (owned by core)
  private(set) var rev: UInt64 = 0
  private(set) var executionRev: UInt64 = 0
  private(set) var confirmedProject: JSONValue = .object([])
  private(set) var confirmedLayout: JSONValue = .object([])
  private(set) var confirmedDefinitions: [String: JSONValue] = [:]
  private(set) var confirmedDefinitionMeta: [String: JSONValue] = [:]
  /// The Agent on the canvas: what it reads, writes and threads.
  let presence = AgentPresence()
  /// The helpers' birds while the Agent has work delegated (one per helper),
  /// and which helper owns which root-level Card.
  var helperPresences: [AgentPresence] = []
  var cardHelper: [String: String] = [:]
  /// Externals by folder ("externals/<name>/", nested "…/externals/<name>/").
  private(set) var externals: [String: ExternalProject] = [:]
  @ObservationIgnored private var externalsKey = ""
  private(set) var sources: [String: (hash: String, size: UInt64)] = [:]
  private(set) var resolution: [CardKey: ResolvedCard] = [:]
  private(set) var diagnostics: [Diagnostic] = []
  private(set) var executionServices: [Tsugumi_RPC_ExecutionServiceT] = []

  // Draft: edits not saved yet. Every edit is saved at once (`edit`), so the
  // draft holds only what is on its way to the core, or what could not be
  // saved (a run in progress, a lost connection).
  var project: JSONValue = .object([])
  var layout: JSONValue = .object([])
  private(set) var undoStack: [UndoEntry] = []
  private(set) var redoStack: [UndoEntry] = []
  private(set) var saveError: String?
  var saving: Bool { dirty }
  /// Last user-facing problem of an action (run refused, import failed, ...).
  var cacheUsage: Tsugumi_RPC_ProjectCacheUsageResultT?
  var notice: String?
  /// The Draft being written on the canvas (geometry opens its Card up).
  var inlineDraft: InlineDraft?
  @ObservationIgnored var draftFinishTask: Task<CardKey?, Never>?
  var hasUnsavedEdits: Bool {
    dirty || draftFinishTask != nil || inlineDraft.map { $0.name != $0.initialName || $0.content != $0.initialContent } == true
  }

  /// A Draft being written on its Card: its name and content as typed so far.
  struct InlineDraft {
    var card: CardKey
    var name: String
    var content: String
    var initialName: String
    var initialContent: String
    var initialMeta: JSONValue?
    let revision: RevisionEdit
  }

  struct UndoEntry { let operation: Tsugumi_Host_ProjectEditsT }

  @ObservationIgnored private let documentRevisions = DocumentRevisions()
  @ObservationIgnored private var operationTask: Task<Tsugumi_RPC_SaveResultT, Error>?
  var operateProject: (Tsugumi_Host_ProjectOperateRequestT) async throws -> Tsugumi_RPC_SaveResultT = {
    try await CoreHostClient.shared.operateProject($0)
  }
  private var submittedOperations = 0
  var dirty: Bool { submittedOperations > 0 }
  var computationDirty: Bool { dirty }
  var syncState: SyncState {
    if !connected { return .offline }
    if let e = saveError { return .failed(e) }
    return dirty ? .saving : .saved
  }
  var definitions: [String: JSONValue] { confirmedDefinitions }
  var definitionMeta: [String: JSONValue] { confirmedDefinitionMeta }
  var sourcePaths: [String] { sources.keys.sorted() }

  // View state saved with ViewSave: the active tab's scope and selection.
  private(set) var initialScope: [String] = []
  private(set) var initialSelection: Set<CardKey> = []
  var cameras: [CardKey: (x: Double, y: Double, zoom: Double)] = [:]
  /// A scope's camera, read without observing it: the canvas lays itself out
  /// with it and follows its moves by a transform (CanvasPan), instead of
  /// drawing every Card again at every move.
  func cameraUnobserved(_ k: CardKey) -> (x: Double, y: Double, zoom: Double)? { _cameras[k] }
  @ObservationIgnored private var viewRev: UInt64 = 0
  @ObservationIgnored private var viewSaveTask: Task<Void, Never>?

  // Versions (git): the graph as last read, and whether this project is new
  // (made or copied by the app), so it becomes a repository when it connects.
  var versionComments: [String: [Tsugumi_Host_VersionCommentT]] = [:]
  var versionGraph: Tsugumi_RPC_VersionGraphT?
  /// A version's Cards and wires (a commit never changes: read once).
  var versionCanvases: [String: Tsugumi_Host_VersionCanvasT] = [:]
  var versionNoteBusy: Set<String> = []
  var versionNoteErrors: [String: String] = [:]
  var versionNotes: [String: String] = [:]
  var versionNoteRequests: [String: UUID] = [:]
  @ObservationIgnored var makeRepositoryOnConnect = false

  // Runs
  private var runProvenance: [String: Tsugumi_RPC_RunStatusT] = [:]
  private var loadingProvenance: Set<String> = []
  private(set) var runStatus: Tsugumi_RPC_RunStatusT? {
    didSet { if let status = runStatus, let id = status.runId { runProvenance[id] = status } }
  }
  func markForRun(_ runId: String) -> String? { runMark(runProvenance[runId]) }
  private(set) var cardStates: [CardKey: CardRunState] = [:]
  private(set) var displays: [CardKey: DisplaySnapshot] = [:]
  private(set) var lastFinished: Date?
  private(set) var remoteHistoryRevision = 0
  private(set) var batchHistoryRevision = 0
  private var runBatches: [CardKey: Tsugumi_RPC_RunBatchT] = [:]
  /// Invocations of the current (or last) run, for the timeline.
  private(set) var timeline: [TimelineEntry] = []
  var runActive: Bool {
    guard let s = runStatus?.state else { return false }
    return [.queued, .preparing, .running, .cancelling].contains(s)
  }
  var currentRunId: String? { runStatus?.runId }
  /// Mac clock minus core clock (ms), estimated from event timestamps: the
  /// smallest difference seen (an event is never received before it happened).
  private(set) var coreClockOffset: Int64?
  /// The core clock now (ms since 1970), for elapsed times of a running run.
  func coreNow() -> Int64 { Int64(Date().timeIntervalSince1970 * 1000) - (coreClockOffset ?? 0) }
  private func sampleClock(_ coreMs: Int64?) {
    guard let coreMs else { return }
    let d = Int64(Date().timeIntervalSince1970 * 1000) - coreMs
    if coreClockOffset == nil || d < coreClockOffset! { coreClockOffset = d }
  }
  private(set) var logRevision = 0  // bumps when new log bytes are available

  init(identity: ProjectIdentity, environments: EnvironmentManager) {
    self.identity = identity
    self.environments = environments
    terminals = ProjectTerminals(identity: identity) { [environments] identity, rows, cols in
      try await environments.openTerminal(identity, rows: rows, cols: cols)
    }
    // A stable view id per project and machine keeps camera/selection across launches.
    let keyName = "view.\(identity.id)"
    if let s = UserDefaults.standard.string(forKey: keyName), let id = UUID(uuidString: s) {
      viewId = id
    } else {
      viewId = UUID()
      UserDefaults.standard.set(viewId.uuidString, forKey: keyName)
    }
  }

  // MARK: - Connection

  @ObservationIgnored private var startTask: Task<Void, Never>?

  func start() {
    guard startTask == nil else { return }
    // Shown as starting at once: the loading card, never a "stopped" page
    // for the moment before the environment reports its state.
    switch environment {
    case .stopped, .incompatibleHistory: environment = .starting("")
    default: break
    }
    startTask = Task { [weak self] in
      guard let self else { return }
      defer { startTask = nil }
      if observerToken == nil, endpointOverride == nil {
        observerToken = await environments.observe(identity) { [weak self] s in
          Task { @MainActor in
            guard let self, !self.closed else { return }
            self.environment = s
          }
        }
      }
      if !Task.isCancelled { await connect() }
    }
  }

  /// The one place `connected` changes: terminals open their shells on it.
  func refreshCacheUsage() async {
    guard let rpc else { return }
    do {
      let request = Tsugumi_Host_HostRuntimeRequestT(); request.action = .command; request.command = "cache-usage"
      let _: Tsugumi_Host_HostRuntimeT = try await CoreHostClient.shared.hostCall(.hostruntimerequest, request)
      let usage = try await rpc.cacheUsage()
      cacheUsage = usage
    } catch { /* A failed measurement never interrupts execution. */ }
  }

  private func setConnected(_ value: Bool) {
    if !value { documentRevisions.disconnect(RPCError.disconnected) }
    connected = value
    terminals.environment(connected: value)
  }

  /// The last window detaches its connection and observations. The resident
  /// host keeps project execution alive. The caller first settles unsaved edits.
  func close() async {
    closed = true
    startTask?.cancel()
    terminals.closeAll()
    listenTask?.cancel()
    reconnectTask?.cancel()
    reconnectTask = nil
    await client?.close()
    client = nil
    setConnected(false)
    if let t = observerToken {
      await environments.stopObserving(identity, t)
      observerToken = nil
    }
    // The resident host keeps project execution alive after this view closes.
    environment = .stopped
  }

  /// A tsugumid to use instead of starting the environment (tests).
  @ObservationIgnored var endpointOverride: Endpoint?
  /// The last connection failed before the project could be read (tsugumid was not reached), not while reading it.
  var connectionFailed = false

  var displayGeometries: Tsugumi_Host_DisplayGeometriesT?

  private func connect(endpoint: Endpoint? = nil) async {
    guard !closed else { return }
    connectionMessage = nil
    do {
      try await refreshProjectTrust()
      let ep: Endpoint
      if let o = endpoint ?? endpointOverride {
        ep = o
      } else {
        ep = try await environments.start(identity)
      }
      try Task.checkCancellation()
      let c = try await reach(ep)
      if Task.isCancelled || closed { await c.close(); return }
      displayGeometries = try await CoreHostClient.shared.hostCall(
        .displaygeometryrequest, Tsugumi_Host_DisplayGeometryRequestT())
      let opened = try await c.open(viewId: viewId)
      if Task.isCancelled || closed { await c.close(); return }
      client = c
      ModuleSources.register(c)
      receiveProjectSnapshot(opened.snapshot, projectId: ep.projectId)
      subscription = opened.subscriptionId
      lastSeq = opened.seq ?? 1
      listen(c)
      if makeRepositoryOnConnect {
        makeRepositoryOnConnect = false
        _ = try? await c.versionInit(opId: UUID(), origin: "ui")
      }
      await refreshVisibleDisplays()
      await restoreCardStates(c)
    } catch {
      guard !Task.isCancelled, !closed else { return }
      setConnected(false)
      connectionFailed = error is Unreached
      connectionMessage = "\(error)"
      if let launcher = error as? LauncherError, case .incompatibleHistory(let issue) = launcher {
        environment = .incompatibleHistory(issue)
      }
    }
  }

  /// A failure to reach the tsugumid, as opposed to one while reading the project.
  struct Unreached: Error, CustomStringConvertible {
    let underlying: Error
    var description: String { "\(underlying)" }
  }

  private func reach(_ ep: Endpoint) async throws -> RPCClient {
    let c = RPCClient(endpoint: ep.url)
    do {
      _ = try await withTaskCancellationHandler {
        try await c.connect(token: ep.token, expectedProjectId: nil, client: .macos)
      } onCancel: { Task { await c.close() } }
      return c
    } catch {
      await c.close()
      if Self.unreachable(error) { throw Unreached(underlying: error) }
      throw error
    }
  }

  /// The tsugumid was not reached: the connection could not be made or ended
  /// before the hello was answered.
  nonisolated static func unreachable(_ error: Error) -> Bool {
    if case RPCError.disconnected = error { return true }
    guard let e = error as? URLError else { return false }
    return [.cannotConnectToHost, .networkConnectionLost, .timedOut, .cannotFindHost, .notConnectedToInternet].contains(e.code)
  }

  /// Reconnects after a lost connection: Hello -> Open -> re-read state.
  /// Uncommitted edits are kept and saved once connected again (the save is
  /// checked against the revision it was based on, so a change made
  /// elsewhere meanwhile is a conflict, never overwritten).
  func reconnect() {
    guard !closed, attachmentTask == nil, reconnectTask == nil else { return }
    environment = .reconnecting
    listenTask?.cancel()
    reconnectTask = Task {
      defer { reconnectTask = nil }
      await client?.close()
      client = nil
      setConnected(false)
      // Hostd's Start waits for the current environment transition, including
      // a guest restart caused by another chat or client.
      if Task.isCancelled { return }
      await connect()
      if connected || Task.isCancelled { return }
      if case .incompatibleHistory = environment { return }
      if case .failed = environment { return }
      // A completed host start/connection failure remains explicitly retryable.
      environment = .failed(message: String(localized: "tsugumid に再接続できませんでした") + (connectionMessage.map { ": \($0)" } ?? ""),
                            recovery: String(localized: "「再試行」で接続し直します。未保存の変更は保持しています。"), log: "")
    }
  }

  /// Hostd owns sharing, restart, readiness and registration. Suspend the old
  /// listener before asking, then open this view on the returned new endpoint.
  func attachFolders(_ paths: [String]) async throws -> [(folder: String, resource: String)] {
    // Several chats/windows share this session; each opens it only after the
    // preceding attachment has finished reconnecting its view.
    let previous = attachmentTask
    let id = UUID()
    let task = Task {
      if let previous { _ = try? await previous.value }
      guard !closed else { throw RPCError.disconnected }
      return try await performAttachFolders(paths)
    }
    attachmentID = id
    attachmentTask = task
    defer { if attachmentID == id { attachmentTask = nil; attachmentID = nil } }
    return try await task.value
  }

  private func performAttachFolders(_ paths: [String]) async throws -> [(folder: String, resource: String)] {
    listenTask?.cancel()
    let reconnecting = reconnectTask
    reconnecting?.cancel()
    await reconnecting?.value
    reconnectTask = nil
    await client?.close()
    client = nil
    setConnected(false)
    environment = .starting(EnvironmentStage.connecting.rawValue)
    var registered: [(folder: String, resource: String)]?
    do {
      let (endpoint, folders) = try await environments.attachFolders(identity, paths)
      registered = folders
      await connect(endpoint: endpoint)
      guard connected else { throw RPCError.disconnected }
      return folders
    } catch {
      // Recover this view after either a host rollback or a transient failure
      // to reach its ready endpoint. A completed registration stays successful
      // when the replacement connection has recovered.
      if !connected { await connect() }
      if connected, let registered { return registered }
      throw error
    }
  }

  /// Quit is stopping the host: from here on this view neither reconnects,
  /// restarts its environment nor takes in its state, so the stop is not
  /// queued behind that work. Nothing here waits.
  func endForQuit() {
    closed = true
    startTask?.cancel()
    listenTask?.cancel()
    reconnectTask?.cancel()
    reconnectTask = nil
  }

  func stopEnvironment() {
    startTask?.cancel()
    // A stopped environment stays stopped: its connection's end is not a
    // loss to reconnect from (that would start it again).
    listenTask?.cancel()
    reconnectTask?.cancel()
    reconnectTask = nil
    Task {
      await client?.close()
      client = nil
      setConnected(false)
      try? await environments.stop(identity)
    }
  }

  /// Notifications are taken in as they arrive (core never waits for the
  /// UI) and applied in batches at most once per frame: a run with thousands
  /// of short invocations causes one view update per batch, not per event.
  private func listen(_ c: RPCClient) {
    let inbox = NotificationInbox()
    let receiver = Task.detached {
      for await event in c.notifications { inbox.put(event) }
      inbox.finish()
    }
    listenTask = Task { [weak self] in
      while !Task.isCancelled {
        let (batch, finished) = await inbox.take()
        guard let self else { receiver.cancel(); return }
        if !batch.isEmpty { self.notificationBatches += 1 }
        // A gap ends the batch: the rest is read again after reconnecting.
        for event in batch where await self.handle(event) == false { break }
        if finished { break }
        // Let the frame that shows this batch happen before the next one.
        try? await Task.sleep(for: .milliseconds(16))
      }
      receiver.cancel()
      guard let self, !Task.isCancelled else { return }
      self.setConnected(false)
      self.reconnect()
    }
  }

  /// How many notifications the UI has taken in, and in how many batches (UI script `state`).
  private(set) var notificationCount = 0
  private(set) var notificationBatches = 0

  /// False when the state cannot be trusted any more (the caller stops).
  @discardableResult
  private func handle(_ e: NotificationEvent) async -> Bool {
    notificationCount += 1
    guard e.subscriptionId == subscription else { return true }  // stale subscription
    if e.seq != lastSeq + 1 {
      // A gap means we cannot trust our state: open again.
      reconnect()
      return false
    }
    lastSeq = e.seq
    switch e.payload.type {
    case .docpatched:
      if let d = (e.payload.value as? Tsugumi_RPC_DocPatchedT)?.delta { applyDelta(d) }
    case .runevent:
      if let r = e.payload.value as? Tsugumi_RPC_RunEventT { applyRunEvent(r) }
    case .displayupdated:
      if let d = (e.payload.value as? Tsugumi_RPC_DisplayUpdatedT)?.display { applyDisplay(d) }
    case .logavailable:
      logRevision &+= 1
    default:
      break
    }
    return true
  }

  // MARK: - Applying core state

  /// Adopt an opened host project before accepting operations from a client.
  func receiveProjectSnapshot(_ snapshot: Tsugumi_RPC_ProjectSnapshotT, projectId: String) {
    self.projectId = projectId
    apply(snapshot: snapshot)
    setConnected(true)
  }

  private func apply(snapshot s: Tsugumi_RPC_ProjectSnapshotT) {
    rev = s.rev ?? 0
    documentRevisions.reset(rev)
    executionRev = s.executionRev ?? 0
    executionServices = s.executionServices.compactMap { $0 }
    confirmedProject = (try? JSONValue.parse(s.projectJson)) ?? .object([])
    confirmedLayout = (try? JSONValue.parse(s.layoutJson)) ?? .object([])
    confirmedDefinitions = [:]
    confirmedDefinitionMeta = [:]
    for d in s.definitions.compactMap({ $0 }) {
      confirmedDefinitions[d.id] = try? JSONValue.parse(d.cardJson)
      if let m = d.metaJson { confirmedDefinitionMeta[d.id] = try? JSONValue.parse(m) }
    }
    sources = [:]
    for src in s.sources.compactMap({ $0 }) { sources[src.path] = (src.hash ?? "", src.sizeBytes ?? 0) }
    refreshExternals()
    refreshDerivedLayout()
    applyResolution(s.resolution.compactMap { $0 }, s.diagnostics.compactMap { $0 })
    if currentRunId != s.run.runId { runBatches = [:] }
    runStatus = s.run
    viewRev = s.viewRev ?? 0
    if let json = s.viewJson, let v = try? JSONDecoder().decode(ClientViewState.self, from: Data(json.utf8)) {
      initialScope = v.scope
      for cam in v.cameras where cam.valid { cameras[key(cam.scope)] = (cam.x, cam.y, cam.zoom) }
      initialSelection = Set(v.selected_cards.map { key($0) })
    }
    project = confirmedProject
    layout = confirmedLayout
    documentRevisions.drain { delta in applyOrderedDelta(delta) }
  }

  private func applyResolution(_ res: [Tsugumi_RPC_ResolvedCardT], _ diags: [Tsugumi_Data_DiagnosticT]) {
    var r: [CardKey: ResolvedCard] = [:]
    var all = diags.map(Diagnostic.init)
    for rc in res {
      let p = rc.card.path
      let port = { (x: Tsugumi_Data_PortT) -> PortInfo in
        PortInfo(name: x.name, kind: x.kind, optional: x.optional_ ?? false, rootType: x.rootType, executionShape: x.executionShape)
      }
      let d = rc.diagnostics.compactMap { $0 }.map(Diagnostic.init)
      r[key(p)] = ResolvedCard(path: p, inputs: rc.inputs.compactMap { $0 }.map(port),
                               outputs: rc.outputs.compactMap { $0 }.map(port), diagnostics: d,
                               nixReferences: rc.nixReferences.compactMap { $0 }.map {
                                 CardNixReference(source: key(p), target: key($0.target.path), name: $0.name, valid: $0.valid)
                               })
      all += d
    }
    resolution = r
    diagnostics = all
    // A result belongs to a Card of the document. A Card that comes back asks for
    // its own again: displayedCards changed (refreshVisibleDisplays).
    for k in displays.keys where r[k] == nil { displays[k] = nil }
  }

  /// What a Card's invocation says about the Card, from a notification or from the run's record.
  private func noteInvocation(_ inv: Tsugumi_RPC_InvocationSummaryT) {
    let k = key(inv.card.path)
    let prev = cardStates[k]
    let n = inv.invocation ?? 0
    if prev == nil || prev!.invocation <= n {
      var next = CardRunState(state: inv.state, invocation: n, phase: inv.phase, failure: inv.failure?.diagnostic)
      next.batchActivation = inv.batchActivation
      next.executionState = inv.executionState
      if (inv.state == .running || inv.state == .preparing) && inv.transferDirection?.isEmpty == false && inv.transferDone < inv.transferTotal {
        next.transfer = String(localized: "ファイル転送: \(inv.transferName ?? "") (\(inv.transferDone)/\(inv.transferTotal) bytes)")
      }
      // Each notification carries the invocation's stages so far.
      next.stages = inv.stages.compactMap { $0 }.compactMap { t in
        Stage(phase: t.phase).map { StageRecord(stage: $0, outcome: t.outcome, start: t.start, end: t.end) }
      }
      next.cached = inv.buildCached == true
      next.workerReused = inv.workerReused == true
      next.sent = Set(inv.outputsSent.compactMap { $0 })
      next.sentInRun = (prev?.sentInRun ?? []).union(next.sent)
      next.startedAt = inv.startedAt
      next.endedAt = inv.endedAt
      next.readyAt = inv.readyAt
      cardStates[k] = next
    }
  }

  private func noteBatch(_ batch: Tsugumi_RPC_RunBatchT) {
    let card = key(batch.card.path)
    if let current = runBatches[card] {
      guard batch.activation > current.activation ||
        (batch.activation == current.activation && (!current.complete || batch.complete)) else { return }
    }
    runBatches[card] = batch
    if batch.executionList, batch.complete, !batch.failed, batch.invocationCount == 0 {
      // Nix preparation can use a provisional invocation before it finds an empty list.
      if cardStates[card]?.batchActivation == batch.activation { cardStates[card] = nil }
      timeline.removeAll { $0.card == card && $0.batchActivation == batch.activation }
    }
  }

  func executionBatch(_ card: CardKey) -> Tsugumi_RPC_RunBatchT? {
    guard let batch = runBatches[card], currentRunId != nil, runStatus?.executionRev == executionRev, !computationDirty else { return nil }
    return batch
  }

  func canvasCardState(_ card: CardKey) -> Tsugumi_Data_CardState? {
    if let batch = executionBatch(card), batch.executionList {
      if runStatus?.state == .cancelled, !batch.complete || batch.failed { return .cancelled }
      return batch.complete ? (batch.failed ? .failed : .succeeded) : .running
    }
    return cardStates[card]?.state
  }

  func executionWire(_ card: CardKey, _ port: String, output: Bool = true) -> (executionList: Bool, count: UInt32?) {
    if output, kind(card) == .subgraph, let inner = boundarySource(card, output: port) {
      return executionWire(inner.0, inner.1)
    }
    let batch = output ? executionBatch(card) : nil
    if let emitted = batch?.outputs.compactMap({ $0 }).first(where: { $0.port == port }) {
      return (emitted.executionList, emitted.executionList ? emitted.count : nil)
    }
    let ports = output ? resolution[card]?.outputs : resolution[card]?.inputs
    let knownList = ports?.first(where: { $0.name == port })?.executionShape == .list
    return (knownList || (batch?.executionList == true && kind(card) != .collect), nil)
  }

  /// The Cards of the last run, for a project just opened: their notifications were sent before it was.
  private func restoreCardStates(_ c: RPCClient) async {
    guard let run = runStatus?.runId, let page = try? await c.latestInvocations(runId: run),
      runStatus?.runId == run  // a run started meanwhile has its own states
    else { return }
    for inv in page.records.compactMap({ $0 }) { noteInvocation(inv) }
    for batch in page.batches.compactMap({ $0 }) { noteBatch(batch) }
  }

  func applyDelta(_ d: Tsugumi_RPC_DocumentDeltaT) {
    documentRevisions.receive(d) { delta in
      applyOrderedDelta(delta)
    }
  }

  private func applyOrderedDelta(_ d: Tsugumi_RPC_DocumentDeltaT) {
    guard d.baseRev == rev else {
      if let r = d.rev, r <= rev { return }  // already applied (our own save reply)
      reconnect()
      return
    }
    // The Cards that send displays, before this change (definitions and
    // resolution are replaced below).
    let shown = displayedCards
    // What the Agent's save changed, seen against the document before it.
    let presenceBefore = d.origin == "agent" ? presenceSnapshot() : nil
    rev = d.rev ?? rev
    executionRev = d.executionRev ?? executionRev
    if let pj = d.projectJson, let v = try? JSONValue.parse(pj) { confirmedProject = v }
    if let lj = d.layoutJson, let v = try? JSONValue.parse(lj) { confirmedLayout = v }
    for def in d.definitionsRemoved.compactMap({ $0 }) {
      confirmedDefinitions[def] = nil
      confirmedDefinitionMeta[def] = nil
    }
    for def in d.definitionsUpsert.compactMap({ $0 }) {
      confirmedDefinitions[def.id] = try? JSONValue.parse(def.cardJson)
      confirmedDefinitionMeta[def.id] = def.metaJson.flatMap { try? JSONValue.parse($0) }
    }
    for s in d.sourcesUpsert.compactMap({ $0 }) { sources[s.path] = (s.hash ?? "", s.sizeBytes ?? 0) }
    for p in d.sourcesRemoved.compactMap({ $0 }) { sources[p] = nil }
    refreshExternals()
    refreshDerivedLayout()
    applyResolution(d.resolution.compactMap { $0 }, d.diagnostics.compactMap { $0 })
    if let presenceBefore { notePresence(d, before: presenceBefore) }
    // A Draft implemented (or any Card that became code) sends its displays too.
    if displayedCards != shown { Task { await refreshVisibleDisplays() } }
    project = confirmedProject
    layout = confirmedLayout
  }

  func applyRunEvent(_ e: Tsugumi_RPC_RunEventT) {
    guard let v = e.value else { return }
    switch v.type {
    case .runacceptedevent:
      for bird in presences { bird.running() }
      if let s = (v.value as? Tsugumi_RPC_RunAcceptedEventT)?.status, currentRunId != s.runId {
        runBatches = [:]
        runStatus = s
        cardStates = [:]
        timeline = []
        sampleClock(s.acceptedAt)
      }
    case .runstatuschanged:
      if let s = (v.value as? Tsugumi_RPC_RunStatusChangedT)?.status, s.runId == runStatus?.runId || runStatus == nil {
        runStatus = s
      }
    case .runcardchanged:
      if let inv = (v.value as? Tsugumi_RPC_RunCardChangedT)?.invocation {
        if inv.batchActivation != nil {
          batchHistoryRevision += 1
          guard e.runId == currentRunId else { break }
          if let batch = runBatches[key(inv.card.path)], batch.activation == inv.batchActivation,
             batch.executionList, batch.complete, !batch.failed, batch.invocationCount == 0 { break }
        }
        if inv.executionState != .unspecified {
          remoteHistoryRevision += 1
          logRevision += 1
          // Recovery also publishes events for runs already in history.
          guard e.runId == currentRunId else { break }
        }
        noteInvocation(inv)
        sampleClock(inv.endedAt ?? (inv.state == .running ? inv.stages.last??.start : nil))
        recordTimeline(key(inv.card.path), inv)
      }
    case .runbatchchanged:
      if let batch = (v.value as? Tsugumi_RPC_RunBatchChangedT)?.batch {
        batchHistoryRevision += 1
        if e.runId == currentRunId { noteBatch(batch) }
      }
    case .runfinished:
      if let s = (v.value as? Tsugumi_RPC_RunFinishedT)?.summary.status { runStatus = s }
      lastFinished = Date()
    default:
      break
    }
  }

  func applyDisplay(_ d: Tsugumi_Data_DisplayT) {
    let k = key(d.card.path)
    let rev = d.revision ?? 0
    if let old = displays[k], old.runId == d.runId, old.revision >= rev { return }  // stale frame
    if let display = recordedDisplay(d) {
      displays[k] = display
      // A reconnected canvas may receive a display from an older run.
      if let client, runProvenance[display.runId] == nil, !loadingProvenance.contains(display.runId) {
        loadingProvenance.insert(display.runId)
        Task {
          defer { loadingProvenance.remove(display.runId) }
          if let record = try? await client.record(runId: display.runId, card: path(k), invocation: display.invocation) {
            runProvenance[display.runId] = record.run.status
          }
        }
      }
    }
  }

  private func recordTimeline(_ k: CardKey, _ inv: Tsugumi_RPC_InvocationSummaryT) {
    let n = inv.invocation ?? 0
    let stages = TimelineEntry.stages(inv.stages)
    if let i = timeline.firstIndex(where: { $0.card == k && $0.invocation == n }) {
      timeline[i].state = inv.state
      timeline[i].startedAt = inv.startedAt ?? timeline[i].startedAt
      timeline[i].endedAt = inv.endedAt ?? timeline[i].endedAt
      if !stages.isEmpty { timeline[i].stages = stages }
    } else {
      timeline.append(TimelineEntry(card: k, invocation: n, batchActivation: inv.batchActivation, state: inv.state, startedAt: inv.startedAt, endedAt: inv.endedAt,
                                    stages: stages))
    }
  }

  // MARK: - Editing (draft, undo/redo)

  /// Confirmed definitions as definition files (card and meta).
  var confirmedDefinitionDrafts: [String: DefinitionDraft] {
    var d: [String: DefinitionDraft] = [:]
    for (id, c) in confirmedDefinitions { d[id] = DefinitionDraft(card: c, meta: confirmedDefinitionMeta[id]) }
    return d
  }

  var definitionDrafts: [String: DefinitionDraft] { confirmedDefinitionDrafts }
  var effectiveExternals: [String: ExternalProject] { externals }

  /// Changes an external's documents in the draft.
  func updateExternal(_ prefix: String, _ fn: (inout ExternalProject) -> Void) {
    guard !prefix.isEmpty, let before = externals[prefix] else { return }
    var after = before; fn(&after)
    editJSON(prefix + "tsugumi.json", old: before.project, new: after.project)
    editJSON(prefix + "layout.json", old: before.layout, new: after.layout)
    for (id, d) in after.definitions {
      let old = before.definitions[id], dir = prefix + "definitions/" + id + "/"
      editJSON(dir + "_card.json", old: old?.card, new: d.card)
      editJSON(dir + "_meta.json", old: old?.meta, new: d.meta)
    }
  }

  /// Applies an edit and saves it at once. One call is one step of undo. The
  /// change is computed when its turn comes, from the document the earlier
  /// actions left: a quick second edit of the same value is not a conflict.
  func edit(_ change: @escaping (ProjectSession) -> Void) {
    let task = submitOperation(.projectedits, body: { self.mutation(change) })
    Task { @MainActor in
      do { _ = try await task.value }
      catch { notice = error.localizedDescription }
    }
  }

  @ObservationIgnored private var commandEdits: [Tsugumi_Host_ProjectEditT]?

  func jsonEdit(_ file: String, old: JSONValue?, new: JSONValue?) -> Tsugumi_Host_ProjectEditT {
    let value = Tsugumi_Host_ProjectJSONEditT()
    value.filePath = file
    value.oldValue = Tsugumi_Host_ProjectValueT(); value.oldValue.present = old != nil; value.oldValue.json = old?.serialized()
    value.newValue = Tsugumi_Host_ProjectValueT(); value.newValue.present = new != nil; value.newValue.json = new?.serialized()
    let edit = Tsugumi_Host_ProjectEditT()
    edit.value = Tsugumi_Host_ProjectEditValueUnion(value, type: .projectjsonedit)
    return edit
  }

  private func editJSON(_ file: String, old: JSONValue?, new: JSONValue?) {
    guard old != new else { return }
    let edit = jsonEdit(file, old: old, new: new)
    if commandEdits != nil { commandEdits!.append(edit) }
    else { queueFields { [edit] } }
  }

  func appendEdits(_ edits: [Tsugumi_Host_ProjectEditT]) {
    precondition(commandEdits != nil)
    commandEdits!.append(contentsOf: edits)
  }

  private func mutation(_ change: (ProjectSession) -> Void) -> Tsugumi_Host_ProjectEditsT {
    precondition(commandEdits == nil)
    commandEdits = []
    defer { commandEdits = nil }
    change(self)
    let body = Tsugumi_Host_ProjectEditsT()
    body.edits = commandEdits!.map(Optional.some)
    return body
  }

  func applyMutation(_ change: (ProjectSession) -> Void) async throws -> UInt64 {
    let body = mutation(change)
    return try await operate(.projectedits, body: body).rev ?? rev
  }

  func setProjectField(_ name: String, _ value: JSONValue) {
    var after = project; after[name] = value
    editJSON("tsugumi.json", old: project, new: after)
  }

  func replaceDocuments(_ revision: UInt64, _ documents: [(String, JSONValue?)]) async throws -> UInt64 {
    let body = Tsugumi_Host_ProjectReplaceDocumentsT()
    body.expectedRev = revision
    body.documents = documents.map { path, value in
      let document = Tsugumi_Host_ProjectDocumentT()
      document.path = path; document.present = value != nil; document.json = value?.serialized()
      return document
    }
    return try await operate(.projectreplacedocuments, body: body).rev ?? rev
  }

  func undo() async { await revert(undo: true) }
  func redo() async { await revert(undo: false) }

  private func revert(undo: Bool) async {
    let task = submitOperation(.projectedits, body: {
      (undo ? self.undoStack : self.redoStack).last?.operation ?? Tsugumi_Host_ProjectEditsT()
    }, record: false, applied: { result in
      let inverse = try self.operationInverse(result)
      if undo { self.undoStack.removeLast() } else { self.redoStack.removeLast() }
      if !inverse.edits.isEmpty {
        let entry = UndoEntry(operation: inverse)
        if undo { self.redoStack.append(entry) } else { self.undoStack.append(entry) }
      }
    })
    do { _ = try await task.value }
    catch { saveError = String(describing: error) }
  }

  func discardDraft() { inlineDraft = nil }

  /// One human action completes when its reply has entered the ordered document.
  @discardableResult
  func operate(_ type: Tsugumi_Host_ProjectOperationBody, body: NativeObject, record: Bool = true) async throws -> Tsugumi_RPC_SaveResultT {
    try await submitOperation(type, body: { body }, record: record).value
  }

  /// Synchronous setters enter the same queue before returning to the canvas.
  private func submitOperation(_ type: Tsugumi_Host_ProjectOperationBody, body: @escaping () throws -> NativeObject, record: Bool = true, applied: ((Tsugumi_RPC_SaveResultT) throws -> Void)? = nil) -> Task<Tsugumi_RPC_SaveResultT, Error> {
    let previous = operationTask
    submittedOperations += 1
    let task = Task { @MainActor in
      defer { submittedOperations -= 1 }
      _ = try? await previous?.value
      guard connected, let projectId else { throw RPCError.disconnected }
      let connection = client
      let request = Tsugumi_Host_ProjectOperateRequestT()
      request.projectId = projectId
      request.opId = UUID().uuidString.lowercased()
      let payload = try body()
      if let edits = payload as? Tsugumi_Host_ProjectEditsT, edits.edits.isEmpty {
        let result = Tsugumi_RPC_SaveResultT(); result.rev = rev
        return result
      }
      request.body = Tsugumi_Host_ProjectOperationBodyUnion(payload, type: type)
      let result = try await operateProject(request)
      guard connected, self.client === connection else { throw RPCError.disconnected }
      if let delta = result.delta { applyDelta(delta) }
      try await documentRevisions.applied(result.rev ?? rev)
      // Source hashes arrive before the documents of Cards in Card.
      var externalGeneration: UInt64
      repeat {
        externalGeneration = externalsGeneration
        await externalsTask?.value
      } while externalGeneration != externalsGeneration
      let inverse = record ? try operationInverse(result) : Tsugumi_Host_ProjectEditsT()
      if record && !inverse.edits.isEmpty {
        let entry = UndoEntry(operation: inverse)
        undoStack.append(entry)
        if undoStack.count > 500 { undoStack.removeFirst() }
        redoStack = []
      }
      try applied?(result)
      saveError = nil
      return result
    }
    operationTask = task
    // A later action may already own operationTask when this one finishes.
    // Keeping the completed tail is harmless and preserves submission order.
    return task
  }

  private func operationInverse(_ result: Tsugumi_RPC_SaveResultT) throws -> Tsugumi_Host_ProjectEditsT {
    guard !result.operationData.isEmpty else { throw RPCError.protocolViolation("Missing operation changes") }
    var buffer = ByteBuffer(bytes: result.operationData)
    let root: Tsugumi_Host_ProjectEdits = try getCheckedRoot(byteBuffer: &buffer)
    return root.unpack()
  }

  func fieldEdit(_ file: String, _ selector: [JSONValue], old: JSONValue?, new: JSONValue?) -> Tsugumi_Host_ProjectEditT {
    let field = Tsugumi_Host_ProjectFieldEditT()
    field.filePath = file
    field.selector = selector.map { step in
      let value = Tsugumi_Host_ProjectSelectorT()
      if let name = step.string { value.field = name }
      else { value.matchJson = step.serialized() }
      return value
    }
    field.oldValue = Tsugumi_Host_ProjectValueT()
    field.oldValue.present = old != nil; field.oldValue.json = old?.serialized()
    field.newValue = Tsugumi_Host_ProjectValueT()
    field.newValue.present = new != nil; field.newValue.json = new?.serialized()
    let edit = Tsugumi_Host_ProjectEditT()
    edit.value = Tsugumi_Host_ProjectEditValueUnion(field, type: .projectfieldedit)
    return edit
  }

  func operateFields(_ edits: [Tsugumi_Host_ProjectEditT]) async throws {
    if edits.isEmpty { return }
    let body = Tsugumi_Host_ProjectEditsT()
    body.edits = edits.map(Optional.some)
    try await operate(.projectedits, body: body)
  }

  func enqueueOperation(_ type: Tsugumi_Host_ProjectOperationBody, body: NativeObject) -> Task<Void, Never> {
    let task = submitOperation(type, body: { body })
    return Task { @MainActor in
      do { _ = try await task.value }
      catch { notice = error.localizedDescription }
    }
  }

  func queueFields(_ edits: @escaping () throws -> [Tsugumi_Host_ProjectEditT]) {
    let task = submitOperation(.projectedits, body: {
      let body = Tsugumi_Host_ProjectEditsT()
      body.edits = try edits().map(Optional.some)
      return body
    })
    Task { @MainActor in
      do { _ = try await task.value }
      catch { notice = error.localizedDescription }
    }
  }

  /// Wait for submitted actions. Rejected editor text remains with its editor.
  func save() async { _ = try? await operationTask?.value }

  struct ReloadApproval: Identifiable {
    let id = UUID()
    let baseRev: UInt64
    let permissions: [String]
  }
  var reloadApproval: ReloadApproval?
  private(set) var reloadingExternal = false

  func answerReloadApproval(_ allow: Bool, selected: Set<String>) async {
    let request = reloadApproval
    reloadApproval = nil
    if allow, let request { await reloadExternal(approved: request.permissions.filter { selected.contains($0) }, baseRev: request.baseRev) }
  }

  func reloadExternal(approved: [String] = [], baseRev: UInt64? = nil) async {
    guard client != nil, !reloadingExternal else { return }
    let base = baseRev ?? rev
    reloadingExternal = true
    defer { reloadingExternal = false }
    do {
      let body = Tsugumi_Host_ProjectReloadT()
      body.expectedRev = base; body.approvedPermissions = approved
      try await operate(.projectreload, body: body, record: false)
      saveError = nil
    } catch let failure as RPCFailure where failure.kind == .approvalRequired && !failure.permissions.isEmpty {
      reloadApproval = ReloadApproval(baseRev: base, permissions: Array(Set(approved + failure.permissions)).sorted())
      saveError = nil
    } catch {
      saveError = "\(error)"
    }
  }

  // MARK: - View state

  func saveViewSoon(scope: [String], selection: Set<CardKey>) {
    viewSaveTask?.cancel()
    viewSaveTask = Task {
      try? await Task.sleep(for: .milliseconds(600))
      guard !Task.isCancelled, let client else { return }
      let v = ClientViewState(scope: scope, cameras: cameras.map { k, c in
        ClientViewState.Camera(scope: path(k), x: c.x, y: c.y, zoom: c.zoom)
      }, selectedCards: selection.sorted().map { path($0) })
      do {
        let r = try await client.saveView(viewId: viewId, baseRev: viewRev, view: try v.json())
        viewRev = r.viewRev ?? viewRev
      } catch let f as RPCFailure where f.kind == .revisionConflict {
        viewRev = f.currentRev ?? viewRev
      } catch {}
    }
  }

  /// Cards whose displays the canvas shows: what a Card shows is up to its
  /// code (the display SDK), so every Card that can run may send displays.
  var displayedCards: Set<CardKey> { Set(resolution.keys.filter { kind($0) == .code }) }

  /// A Card that took a name another Card had starts without that Card's result and run state.
  func forgetResult(_ k: CardKey) {
    displays[k] = nil
    cardStates[k] = nil
  }

  /// Why a Card that ran has no result to show.
  func noResultNote(_ k: CardKey) -> String? {
    switch cardStates[k]?.state {
    case .failed: return String(localized: "失敗したので結果はありません")
    case .blocked: return String(localized: "上流の Card が失敗したので実行していません")
    default: return nil
    }
  }

  /// Looks a flake up for nixSources (nothing is saved).
  func resolveNixSource(ref: String) async throws -> Tsugumi_RPC_NixSourceResolvedT {
    guard let client else { throw RPCError.disconnected }
    return try await client.resolveNixSource(ref: ref)
  }

  func refreshVisibleDisplays() async {
    guard let client, let subscription else { return }
    try? await client.setVisible(subscription: subscription, cards: displayedCards.sorted().map(path), enabled: true)
  }

  // MARK: - Runs

  func run(card: [String], propagate: Bool) async throws {
    _ = await finishDraftEdit()
    guard inlineDraft == nil else {
      throw RPCFailure(kind: .validationFailed, diagnostic: String(localized: "変更を保存できないため実行できません。"))
    }
    guard let client else { throw RPCError.disconnected }
    if computationDirty { await save() }
    if computationDirty {
      throw RPCFailure(kind: .validationFailed, diagnostic: saveError ?? String(localized: "変更を保存できないため実行できません。"))
    }
    let op = UUID()
    let parallel = UInt32(max(1, min(64, AppPreferences.shared.maxConcurrency)))
    do {
      let accepted = try await client.run(baseRev: rev, opId: op, card: card, propagate: propagate, maxConcurrency: parallel)
      // A fast or empty run may already have reported newer state.
      if currentRunId != accepted.run.runId {
        runBatches = [:]
        runStatus = accepted.run
        cardStates = [:]
        timeline = []
      }
    } catch RPCError.disconnected {
      // Never re-send blindly: look the operation up after reconnecting.
      reconnect()
      throw RPCError.disconnected
    }
  }

  func cancelRun() async {
    guard let client, let id = runStatus?.runId else { return }
    if let s = try? await client.cancel(runId: id) { runStatus = s }
  }

  var rpc: RPCClient? { client }

  func refreshExecutionServices() async {
    guard let client, let snapshot = try? await client.inspect(), self.client === client else { return }
    executionServices = snapshot.executionServices.compactMap { $0 }
  }

  // MARK: - Document helpers

  /// The project a scope's Cards are written in: "" for this project,
  /// "externals/<name>/" (nested further) inside a Card in Card; nil when the
  /// scope is not a Card in Card.
  func projectPrefix(_ scope: [String]) -> String? {
    var prefix = ""
    for id in scope {
      guard let name = externalName(prefix: prefix, id: id) else { return nil }
      prefix += "externals/\(name)/"
    }
    return prefix
  }

  /// An external's documents with the draft applied.
  func external(_ prefix: String) -> ExternalProject? {
    return externals[prefix]
  }

  private func graph(_ scope: [String]) -> JSONValue? {
    guard let prefix = projectPrefix(scope) else { return nil }
    return prefix.isEmpty ? project["canvas"] : external(prefix)?.project["canvas"]
  }

  /// Cards of a scope: this project's, or (inside a Card in Card) the external's.
  func cards(in scope: [String]) -> [JSONValue] { graph(scope)?["cards"]?.array ?? [] }

  func connections(in scope: [String]) -> [JSONValue] { graph(scope)?["connections"]?.array ?? [] }

  /// Mutates a scope's canvas in the draft (inside a Card in Card: its external's).
  func updateGraph(_ scope: [String], _ fn: (inout JSONValue) -> Void) {
    guard let prefix = projectPrefix(scope), let before = prefix.isEmpty ? project : externals[prefix]?.project else { return }
    var after = before, canvas = before["canvas"] ?? .object([])
    fn(&canvas); after["canvas"] = canvas
    editJSON(prefix + "tsugumi.json", old: before, new: after)
  }

  /// The layout a scope's Cards, notes and groups are kept in, and the key of
  /// a Card there (inside a Card in Card: the external's layout, by Card id).
  func layout(of scope: [String]) -> JSONValue {
    guard let prefix = projectPrefix(scope), !prefix.isEmpty else { return layout }
    return external(prefix)?.layout ?? .object([])
  }

  func updateLayout(of scope: [String], _ fn: (inout JSONValue) -> Void) {
    guard let prefix = projectPrefix(scope) else { return }
    let before = layout(of: scope); var after = before
    fn(&after); editJSON(prefix + "layout.json", old: before, new: after)
  }

  /// A Card's key in the layout of its scope.
  func layoutKey(_ k: CardKey) -> String { path(k).count > 1 ? (path(k).last ?? k) : k }

  func instance(_ k: CardKey) -> JSONValue? {
    let p = path(k)
    guard let last = p.last else { return nil }
    return cards(in: Array(p.dropLast())).first { $0["id"]?.string == last }
  }

  /// The Card's definition files with the draft applied (its own project's, or the external's).
  func cardDefinitionDraft(_ k: CardKey) -> DefinitionDraft? {
    let p = path(k)
    guard let id = p.last, let prefix = projectPrefix(Array(p.dropLast())) else { return nil }
    return prefix.isEmpty ? definitionDrafts[id] : external(prefix)?.definitions[id]
  }

  /// The Card's settings (its _card.json).
  func cardDefinition(_ k: CardKey) -> JSONValue? {
    guard let d = cardDefinitionDraft(k), d.card["external"] == nil else { return nil }
    return d.card
  }

  /// The Card's _meta.json.
  func cardMeta(_ k: CardKey) -> JSONValue? { cardDefinitionDraft(k)?.meta }

  /// Sets a Card's definition files in the draft (nil removes them).
  func setCardDefinition(_ k: CardKey, _ d: DefinitionDraft?) {
    guard let dir = definitionDir(k), let d else { return }
    let before = cardDefinitionDraft(k)
    editJSON(dir + "_card.json", old: before?.card, new: d.card)
    editJSON(dir + "_meta.json", old: before?.meta, new: d.meta)
  }

  /// The external project of a Card in Card ("externals/<name>/…"), if it is one.
  func cardExternal(_ k: CardKey) -> String? {
    let p = path(k)
    guard let id = p.last, let prefix = projectPrefix(Array(p.dropLast())) else { return nil }
    return externalName(prefix: prefix, id: id).map { prefix + "externals/\($0)/" }
  }

  private func externalName(prefix: String, id: String) -> String? {
    let card = prefix.isEmpty ? definitions[id] : external(prefix)?.definitions[id]?.card
    return card?["external"]?.string
  }

  /// The folder of the Card's definition, project relative ("definitions/<id>/").
  func definitionDir(_ k: CardKey) -> String? {
    let p = path(k)
    guard let id = p.last, let prefix = projectPrefix(Array(p.dropLast())) else { return nil }
    return prefix + "definitions/\(id)/"
  }

  /// A Card in Card's ports: its external's boundary.
  func boundary(_ k: CardKey) -> JSONValue? {
    guard let ext = cardExternal(k) else { return nil }
    return external(ext)?.project["boundary"]
  }

  /// Changes a Card's _card.json in the draft.
  func updateCard(_ k: CardKey, _ fn: (inout JSONValue) -> Void) {
    guard var d = cardDefinitionDraft(k), d.card["external"] == nil else { return }
    fn(&d.card)
    setCardDefinition(k, d)
  }

  /// Changes a Card's _meta.json in the draft.
  func updateMeta(_ k: CardKey, _ fn: (inout JSONValue) -> Void) {
    guard var d = cardDefinitionDraft(k) else { return }
    var meta = d.meta ?? .object([])
    fn(&meta)
    d.meta = meta
    setCardDefinition(k, d)
  }

  // MARK: - Comments on a Card (its _meta.json)

  /// What the user asks of this Card, in the order written.
  func comments(_ k: CardKey) -> [CardComment] { CardComment.list(in: cardMeta(k)) }

  /// Comments not resolved yet.
  func openComments(_ k: CardKey) -> [CardComment] { comments(k).filter { !$0.resolved } }

  func changeRequests(_ k: CardKey) -> [CardComment] { openComments(k).filter { !$0.finding } }

  /// The Cards of a scope with change requests not resolved yet.
  func cardsWithOpenComments(in scope: [String]) -> [CardKey] {
    cards(in: scope).compactMap { $0["id"]?.string }.map { key(scope + [$0]) }.filter { !changeRequests($0).isEmpty }
  }

  /// Changes comments in the draft, through the same definition setter.
  private func changeComments(_ k: CardKey, _ fn: ([JSONValue]) -> [JSONValue]) {
    guard var d = cardDefinitionDraft(k) else { return }
    var meta = d.meta ?? .object([])
    let list = fn(meta["comments"]?.array ?? [])
    meta["comments"] = list.isEmpty ? nil : .array(list)
    d.meta = meta.object?.isEmpty ?? true ? nil : meta
    setCardDefinition(k, d)
  }

  /// Immediate comment actions have no open working copy.
  private func editComments(_ k: CardKey, _ fn: @escaping ([JSONValue]) -> [JSONValue]) {
    edit { $0.changeComments(k, fn) }
  }

  func addComment(_ k: CardKey, _ text: String, finding: Bool = false) async throws {
    let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !t.isEmpty else { return }
    let id = try await reserveReference("comment", scope: k)
    _ = try await applyMutation { s in s.changeComments(k) { $0 + [.object([("id", .string(id)), ("text", .string(t)), ("author", .string("user")), ("finding", .bool(finding))])] } }
  }

  func setCommentText(_ k: CardKey, id: String, text: String) {
    let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !t.isEmpty else { return }
    changeComments(k) { list in
      if !list.contains(where: { $0["id"]?.string == id }) {
        return list + [.object([("id", .string(id)), ("text", .string(t)), ("author", .string("user"))])]
      }
      return list.map { c in
        guard c["id"]?.string == id else { return c }
        var n = c
        n["text"] = .string(t)
        n["editedBy"] = .string("user")
        return n
      }
    }
  }

  /// Marks a comment by hand. Opening one again takes back what was said about it.
  func setCommentResolved(_ k: CardKey, id: String, _ resolved: Bool) {
    editComments(k) { list in
      list.map { c in
        guard c["id"]?.string == id else { return c }
        var n = c
        n["resolved"] = resolved ? .bool(true) : nil
        n["stateBy"] = .string("user")
        if !resolved {
          n["resolution"] = nil
          n["cards"] = nil
        }
        return n
      }
    }
  }

  func removeComment(_ k: CardKey, id: String) {
    editComments(k) { $0.filter { $0["id"]?.string != id } }
  }

  /// A setting of the Card (its _card.json).
  func cardSetting(_ k: CardKey, _ name: String) -> JSONValue? { cardDefinition(k)?[name] }

  private(set) var derivedLayouts: [String: JSONValue] = [:]
  @ObservationIgnored private var layoutTasks: [String: Task<Void, Never>] = [:]

  @discardableResult
  func refreshDerivedLayout(prefix: String = "") -> Task<Void, Never> {
    let graph = prefix.isEmpty ? confirmedProject : externals[prefix]?.project ?? .object([])
    let saved = prefix.isEmpty ? confirmedLayout : externals[prefix]?.layout ?? .object([])
    let definitions = prefix.isEmpty ? confirmedDefinitionDrafts : externals[prefix]?.definitions ?? [:]
    layoutTasks[prefix]?.cancel()
    let task = Task {
      do {
        let text = try await prepareLayout(graph.serialized(), saved.serialized(),
          definitions.mapValues { $0.card.serialized() })
        let currentGraph = prefix.isEmpty ? confirmedProject : externals[prefix]?.project ?? .object([])
        let currentLayout = prefix.isEmpty ? confirmedLayout : externals[prefix]?.layout ?? .object([])
        let currentDefinitions = prefix.isEmpty ? confirmedDefinitionDrafts : externals[prefix]?.definitions ?? [:]
        guard !Task.isCancelled, graph == currentGraph, saved == currentLayout,
          definitions == currentDefinitions else { return }
        derivedLayouts[prefix] = try JSONValue.parse(text)
      } catch {
        guard !Task.isCancelled else { return }
        derivedLayouts[prefix] = nil
        notice = String(describing: error)
      }
    }
    layoutTasks[prefix] = task
    return task
  }

  /// Waits for the placement inputs currently shown in this scope.
  func waitForPlacement(_ k: CardKey) async -> Bool {
    let scope = Array(path(k).dropLast())
    while true {
      let externals = externalsTask
      if !scope.isEmpty {
        await externals?.value
        if externals != externalsTask { continue }
      }
      guard let prefix = projectPrefix(scope), instance(k) != nil else { return false }
      let saved = layout(of: scope)["cards"]?[layoutKey(k)]
      if saved?["x"]?.double != nil, saved?["y"]?.double != nil { return true }
      let layout = layoutTasks[prefix]
      await layout?.value
      if layout != layoutTasks[prefix] || (!scope.isEmpty && externals != externalsTask) { continue }
      return projectPrefix(scope) == prefix && instance(k) != nil &&
        derivedLayouts[prefix]?["cards"]?[layoutKey(k)] != nil
    }
  }

  func position(_ k: CardKey) -> CGPoint {
    let scope = Array(path(k).dropLast())
    let key = layoutKey(k)
    let saved = layout(of: scope)["cards"]?[key]
    if let x = saved?["x"]?.double, let y = saved?["y"]?.double {
      return CGPoint(x: x, y: y)
    }
    let derived = derivedLayouts[projectPrefix(scope) ?? ""]?["cards"]?[key]
    return CGPoint(x: derived?["x"]?.double ?? 0, y: derived?["y"]?.double ?? 0)
  }

  /// Reads the externals' documents when their sources changed.
  @ObservationIgnored var readExternalFile: ((String, UInt64) async throws -> Data)?
  @ObservationIgnored private var externalsTask: Task<Void, Never>?
  @ObservationIgnored private var externalsGeneration: UInt64 = 0

  @discardableResult
  func refreshExternals() -> Task<Void, Never>? {
    let paths = sources.keys.filter { $0.hasPrefix("externals/") }.sorted()
    let key = paths.map { "\($0)=\(sources[$0]?.hash ?? "")" }.joined(separator: ";")
    guard key != externalsKey else { return externalsTask }
    guard client != nil || readExternalFile != nil else { return nil }
    externalsKey = key
    let client = client
    let readFile = readExternalFile ?? { path, revision in
      guard let client else { throw RPCError.disconnected }
      return try await client.readFile(path: path, atRev: revision)
    }
    var definitionIds: [String: Set<String>] = [:]
    for p in paths {
      guard let split = p.range(of: "/definitions/", options: .backwards) else { continue }
      let parts = p[split.upperBound...].split(separator: "/")
      if parts.count == 2, parts[1] == "_card.json" {
        definitionIds[String(p[..<split.lowerBound]) + "/", default: []].insert(String(parts[0]))
      }
    }
    let atRev = rev
    let hashes = sources.mapValues(\.hash)
    externalsGeneration += 1
    externalsTask = Task { @MainActor in
      var found: [String: ExternalProject] = [:]
      for p in paths where p.hasSuffix("/tsugumi.json") {
        let prefix = String(p.dropLast("tsugumi.json".count))
        // Only folders that are externals of a project: …externals/<name>/tsugumi.json.
        let parts = prefix.split(separator: "/")
        guard parts.count >= 2, parts.count % 2 == 0, parts[parts.count - 2] == "externals" else { continue }
        @Sendable func doc(_ rel: String) async -> JSONValue? {
          guard hashes[rel] != nil, let d = try? await readFile(rel, atRev) else { return nil }
          return try? JSONValue.parse(String(decoding: d, as: UTF8.self))
        }
        async let projectRead = doc(p)
        async let layoutRead = doc(prefix + "layout.json")
        guard let proj = await projectRead else { continue }
        var ext = ExternalProject(project: proj, layout: await layoutRead ?? .object([]), definitions: [:])
        let ids = definitionIds[prefix] ?? []
        for id in ids.sorted() {
          let dir = prefix + "definitions/\(id)/"
          async let metaRead = doc(dir + "_meta.json")
          async let cardRead = doc(dir + "_card.json")
          let (meta, card) = await (metaRead, cardRead)
          if let card { ext.definitions[id] = DefinitionDraft(card: card, meta: meta) }
        }
        found[prefix] = ext
      }
      guard self.externalsKey == key else { return }
      self.externals = found
      for prefix in found.keys { self.refreshDerivedLayout(prefix: prefix) }
      // Cards inside Cards in Card are known now: their displays are wanted too.
      Task { await self.refreshVisibleDisplays() }
    }
    return externalsTask
  }

  func setPosition(_ k: CardKey, _ pt: CGPoint) {
    let lk = layoutKey(k)
    updateLayout(of: Array(path(k).dropLast())) { l in
      var cards = l["cards"] ?? .object([])
      var entry = cards[lk] ?? .object([])
      entry["x"] = .real((pt.x * 10).rounded() / 10)
      entry["y"] = .real((pt.y * 10).rounded() / 10)
      cards[lk] = entry
      l["cards"] = cards
    }
  }
}

/// Notifications waiting for the next UI batch (filled off the main actor).
final class NotificationInbox: @unchecked Sendable {
  private let lock = NSLock()
  private var items: [NotificationEvent] = []
  private var done = false
  private var waiter: CheckedContinuation<Void, Never>?

  func put(_ e: NotificationEvent) {
    lock.lock()
    items.append(e)
    let w = waiter
    waiter = nil
    lock.unlock()
    w?.resume()
  }

  func finish() {
    lock.lock()
    done = true
    let w = waiter
    waiter = nil
    lock.unlock()
    w?.resume()
  }

  /// Everything queued so far (waits until there is something or the stream ended).
  func take() async -> ([NotificationEvent], Bool) {
    while true {
      // (scoped: an async function never holds the lock across a suspension)
      let ready: ([NotificationEvent], Bool)? = lock.withLock {
        guard !items.isEmpty || done else { return nil }
        let batch = items
        items.removeAll(keepingCapacity: true)
        return (batch, done)
      }
      if let ready { return ready }
      await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
        lock.lock()
        if !items.isEmpty || done {
          lock.unlock()
          c.resume()
        } else {
          waiter = c
          lock.unlock()
        }
      }
    }
  }
}
