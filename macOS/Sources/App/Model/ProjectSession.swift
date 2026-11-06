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
}

struct ResolvedCard {
  let path: [String]
  let inputs: [PortInfo]
  let outputs: [PortInfo]
  let diagnostics: [Diagnostic]
}

/// One stage of an invocation as the core recorded it (core clock, ms).
struct StageRecord: Equatable, Hashable {
  var stage: Stage
  var outcome: Tsugumi_Data_StageOutcome
  var start: Int64?
  var end: Int64?
}

/// Latest invocation of a Card in the current run, as the core reports it.
struct CardRunState {
  var state: Tsugumi_Data_CardState
  /// Number of the latest invocation in this run (1, 2, …); 0 = not run.
  var invocation: UInt64
  var phase: Tsugumi_Data_Phase
  var failure: String?
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
  var state: Tsugumi_Data_CardState
  var startedAt: Int64?
  var endedAt: Int64?
  /// Stages of this invocation (core clock), for the timeline.
  var stages: [StageRecord] = []
}

/// Draft of one Card's definition (`_card.json` and `_meta.json`, or for a
/// Card in Card `_link.json`), or its removal.
struct DefinitionDraft: Equatable {
  var card: JSONValue
  var meta: JSONValue?
  var removed = false
  var link: JSONValue? = nil
}

/// A project inside this one (externals/<name>/, nested further): the inner
/// canvas of a Card in Card. A copy owned by this project, edited here like
/// the project's own documents (written back as its files).
struct ExternalProject: Equatable {
  var project: JSONValue
  var layout: JSONValue
  /// Card id -> its definition files (_card.json or _link.json, _meta.json).
  var definitions: [String: DefinitionDraft]
  /// Removed (the folder goes with it).
  var removed = false

  /// Card id -> external name (its own Cards in Card).
  var links: [String: String] { definitions.compactMapValues { $0.link?["external"]?.string } }

  /// The documents as files, relative to the external's folder.
  var files: [String: String] {
    var out = ["tsugumi.json": project.serialized() + "\n", "layout.json": layout.serialized() + "\n"]
    for (id, d) in definitions {
      let dir = "definitions/\(id)/"
      if let l = d.link { out[dir + "_link.json"] = l.serialized() + "\n" } else { out[dir + "_card.json"] = d.card.serialized() + "\n" }
      if let m = d.meta { out[dir + "_meta.json"] = m.serialized() + "\n" }
    }
    return out
  }

  /// Whether a path (relative to the external) is one of its documents.
  static func isDocument(_ rel: String) -> Bool {
    if rel == "tsugumi.json" || rel == "layout.json" { return true }
    let parts = rel.split(separator: "/")
    return parts.count == 3 && parts[0] == "definitions" && ["_card.json", "_link.json", "_meta.json"].contains(parts[2])
  }
}

/// Save/sync state, shown separately from the run state.
enum SyncState: Equatable {
  case saved
  case saving
  case failed(String)
  case offline
  /// Card changes wait until the run ends.
  case waitingForRun
}

enum DisplayValue {
  case text(String)
  /// Markdown with TeX math (the SDK's markdown display, a logger in Markdown mode).
  case markdown(String)
  case scalar(Double, String)
  case lineChart([(id: String, label: String, x: [Double], y: [Double])])
  case table(columns: [String], rows: [[String]])
  case image(mediaType: String, data: Data, alt: String)
  case points(x: [Float], y: [Float], z: [Float], colors: [UInt32])
  case module(name: String, json: String)
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
  let value: DisplayValue
}

/// One open project: shared by every window showing it. Owns the RPC
/// connection, the confirmed document (from tsugumid), the local draft with
/// undo/redo, run progress, displays and logs.
@MainActor
@Observable
final class ProjectSession {
  let identity: ProjectIdentity
  let environments: EnvironmentManager
  let viewId: UUID

  // Connection
  private(set) var environment: EnvironmentState = .stopped
  private(set) var connected = false
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
  @ObservationIgnored private var observerToken: UUID?
  private(set) var projectId: String?

  // Confirmed document (owned by core)
  private(set) var rev: UInt64 = 0
  private(set) var executionRev: UInt64 = 0
  private(set) var confirmedProject: JSONValue = .object([])
  private(set) var confirmedLayout: JSONValue = .object([])
  private(set) var confirmedDefinitions: [String: JSONValue] = [:]
  private(set) var confirmedDefinitionMeta: [String: JSONValue] = [:]
  private(set) var confirmedLinks: [String: JSONValue] = [:]
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

  // Draft: edits not saved yet. Every edit is saved at once (`edit`), so the
  // draft holds only what is on its way to the core, or what could not be
  // saved (a run in progress, a lost connection).
  var project: JSONValue = .object([])
  var layout: JSONValue = .object([])
  var sourceEdits: [String: Data] = [:]  // path -> new bytes
  var sourceRemovals: Set<String> = []
  var definitionEdits: [String: DefinitionDraft] = [:]
  /// Edited externals (by folder), written back as their files.
  var externalEdits: [String: ExternalProject] = [:]
  /// Base hashes chosen by an editor that read the file before this draft
  /// started: the save is checked against what the editor actually saw.
  var sourceBases: [String: String] = [:]
  private(set) var undoStack: [UndoEntry] = []
  private(set) var redoStack: [UndoEntry] = []
  private(set) var saveError: String?
  private(set) var saving = false
  /// Cards renamed inside Cards in Card by edits not saved yet: the save
  /// names them so their run history follows.
  var pendingCardRenames: [(from: [String], to: [String])] = []
  /// Edits wait for the run to end (the core keeps Card changes back while it runs).
  private(set) var savingAfterRun = false
  /// Last user-facing problem of an action (run refused, import failed, ...).
  var notice: String?
  /// The Draft being written on the canvas (geometry opens its Card up).
  var inlineDraft: InlineDraft?

  /// A Draft being written on its Card: its name and content as typed so far.
  struct InlineDraft: Equatable {
    var card: CardKey
    var name: String
    var content: String
  }

  struct Snapshot: Equatable {
    var project: JSONValue
    var layout: JSONValue
    var sources: [String: Data]
    var removals: Set<String>
    var definitions: [String: DefinitionDraft]
    var externals: [String: ExternalProject]
  }

  /// The documents as they look with the draft applied (what an undo entry records).
  struct DocState: Equatable {
    var project: JSONValue
    var layout: JSONValue
    var definitions: [String: DefinitionDraft]
    var externals: [String: ExternalProject]
  }

  /// A source file's content before or after an edit.
  enum SourceValue: Equatable {
    case absent
    case bytes(Data)
    /// The confirmed file (read before the edit is saved, for undo).
    case confirmed
  }

  /// One saved operation: undo applies `before` over what is there now, so
  /// changes made since by the Agent or another window stay.
  final class UndoEntry {
    let before: DocState
    let after: DocState
    var sourcesBefore: [String: SourceValue]
    let sourcesAfter: [String: SourceValue]
    init(before: DocState, after: DocState, sourcesBefore: [String: SourceValue], sourcesAfter: [String: SourceValue]) {
      self.before = before
      self.after = after
      self.sourcesBefore = sourcesBefore
      self.sourcesAfter = sourcesAfter
    }
  }

  /// The confirmed state the current draft was started from: a save merges
  /// the draft's changes from it onto the latest confirmed state.
  private struct DraftBase {
    let rev: UInt64
    let hashes: [String: String]
    let project: JSONValue
    let layout: JSONValue
    let definitions: [String: DefinitionDraft]
  }
  @ObservationIgnored private var draftBase: DraftBase?
  /// Confirmed externals when each external's edit started.
  @ObservationIgnored private var externalBases: [String: ExternalProject?] = [:]
  /// Hashes of the externals' documents as they were read into `externals`.
  @ObservationIgnored private var externalHashes: [String: String] = [:]
  @ObservationIgnored private var saveTask: Task<Void, Never>?
  @ObservationIgnored private var saveAgain = false
  /// Revision the draft is based on (equals `rev` while clean).
  var draftBaseRev: UInt64 { draftBase?.rev ?? rev }

  var dirty: Bool {
    project != confirmedProject || layout != confirmedLayout || !sourceEdits.isEmpty || !sourceRemovals.isEmpty ||
      !definitionEdits.isEmpty || !externalEdits.isEmpty
  }
  var computationDirty: Bool {
    project != confirmedProject || !sourceEdits.isEmpty || !sourceRemovals.isEmpty || !definitionEdits.isEmpty ||
      !externalEdits.isEmpty
  }

  var syncState: SyncState {
    if !connected { return dirty ? .failed(connectionMessage ?? "Not connected") : .offline }
    if saving { return .saving }
    if let e = saveError { return .failed(e) }
    if savingAfterRun && dirty { return .waitingForRun }
    return dirty ? .saving : .saved
  }

  /// Effective definitions: confirmed ones with the draft applied.
  var definitions: [String: JSONValue] {
    var d = confirmedDefinitions
    for (id, e) in definitionEdits { d[id] = e.removed ? nil : e.card }
    return d
  }

  /// Card in Card links of this project's Cards (_link.json), with the draft applied.
  var links: [String: JSONValue] {
    var d = confirmedLinks
    for (id, e) in definitionEdits { d[id] = e.removed ? nil : e.link }
    return d
  }

  var definitionMeta: [String: JSONValue] {
    var d = confirmedDefinitionMeta
    for (id, e) in definitionEdits { d[id] = e.removed ? nil : e.meta }
    return d
  }

  /// Source paths of the draft (confirmed, plus new files, minus removed ones).
  var sourcePaths: [String] {
    Set(sources.keys).union(sourceEdits.keys).subtracting(sourceRemovals).sorted()
  }

  // View state saved with ViewSave: the active tab's scope and selection.
  private(set) var initialScope: [String] = []
  private(set) var initialSelection: Set<CardKey> = []
  var cameras: [CardKey: (x: Double, y: Double, zoom: Double)] = [:]
  @ObservationIgnored private var viewRev: UInt64 = 0
  @ObservationIgnored private var viewSaveTask: Task<Void, Never>?

  // Versions (git): the graph as last read, and whether this project is new
  // (made or copied by the app), so it becomes a repository when it connects.
  var versionGraph: Tsugumi_RPC_VersionGraphT?
  /// A version's Cards and wires (a commit never changes: read once).
  var versionCanvases: [String: Tsugumi_RPC_VersionCanvasT] = [:]
  var versionSummaryBusy: Set<String> = []
  var versionSummaryErrors: [String: String] = [:]
  var versionSummaryTried: Set<String> = []
  @ObservationIgnored var makeRepositoryOnConnect = false

  // Runs
  private(set) var runStatus: Tsugumi_RPC_RunStatusT?
  private(set) var cardStates: [CardKey: CardRunState] = [:]
  private(set) var displays: [CardKey: DisplaySnapshot] = [:]
  private(set) var lastFinished: Date?
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

  func start() {
    // Shown as starting at once: the loading card, never a "stopped" page
    // for the moment before the environment reports its state.
    if case .stopped = environment { environment = .starting("") }
    Task { [weak self] in
      guard let self else { return }
      if observerToken == nil, endpointOverride == nil {
        observerToken = await environments.observe(identity) { [weak self] s in
          Task { @MainActor in self?.environment = s }
        }
      }
      await connect()
    }
  }

  /// Closed for good (the last window left it): the connection ends, the
  /// project's tsugumid stops and its folders are no longer shared with the
  /// VM, so its slot is free. Unsaved edits were saved before (the caller
  /// does not close a dirty session).
  func close() async {
    closed = true
    listenTask?.cancel()
    reconnectTask?.cancel()
    reconnectTask = nil
    await client?.close()
    client = nil
    connected = false
    if let t = observerToken {
      await environments.stopObserving(identity, t)
      observerToken = nil
    }
    if endpointOverride == nil { try? await environments.stop(identity) }
    environment = .stopped
  }

  /// A tsugumid to use instead of starting the environment (tests).
  @ObservationIgnored var endpointOverride: Endpoint?

  private func connect() async {
    connectionMessage = nil
    do {
      let ep: Endpoint
      if let o = endpointOverride { ep = o } else { ep = try await environments.start(identity) }
      let c = RPCClient(endpoint: ep.url)
      _ = try await c.connect(token: ep.token, clientId: UUID(), expectedProjectId: nil)
      let opened = try await c.open(viewId: viewId)
      client = c
      projectId = ep.projectId
      apply(snapshot: opened.snapshot, resetDraft: !dirty)
      subscription = opened.subscriptionId
      lastSeq = opened.seq ?? 1
      connected = true
      listen(c)
      if makeRepositoryOnConnect {
        makeRepositoryOnConnect = false
        _ = try? await c.versionInit(opId: UUID(), origin: "ui")
      }
      if dirty { scheduleSave() }
      await refreshVisibleDisplays()
    } catch {
      connected = false
      connectionMessage = "\(error)"
    }
  }

  /// Reconnects after a lost connection: Hello -> Open -> re-read state.
  /// Uncommitted edits are kept and saved once connected again (the save is
  /// checked against the revision it was based on, so a change made
  /// elsewhere meanwhile is a conflict, never overwritten).
  func reconnect() {
    guard reconnectTask == nil else { return }
    environment = .reconnecting
    listenTask?.cancel()
    reconnectTask = Task {
      defer { reconnectTask = nil }
      await client?.close()
      client = nil
      connected = false
      for attempt in 0..<20 {
        if Task.isCancelled { return }
        await connect()
        if connected { return }
        try? await Task.sleep(for: .milliseconds(min(5000, 250 * (attempt + 1))))
      }
      // Given up: a problem page with its retry, never a loading card forever.
      environment = .failed(message: String(localized: "tsugumid に再接続できませんでした") + (connectionMessage.map { ": \($0)" } ?? ""),
                            recovery: String(localized: "「再試行」で接続し直します。未保存の変更は保持しています。"), log: "")
    }
  }

  /// Starts the environment again (folders shared with it are set when it
  /// starts); unsaved edits stay in the draft.
  func restartEnvironment() async {
    // The old connection's end is not a loss to reconnect from.
    listenTask?.cancel()
    reconnectTask?.cancel()
    reconnectTask = nil
    await client?.close()
    client = nil
    connected = false
    try? await environments.stop(identity)
    await connect()
  }

  func stopEnvironment() {
    Task {
      await client?.close()
      connected = false
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
      self.connected = false
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

  private func apply(snapshot s: Tsugumi_RPC_ProjectSnapshotT, resetDraft: Bool) {
    rev = s.rev ?? 0
    executionRev = s.executionRev ?? 0
    confirmedProject = (try? JSONValue.parse(s.projectJson)) ?? .object([])
    confirmedLayout = (try? JSONValue.parse(s.layoutJson)) ?? .object([])
    confirmedDefinitions = [:]
    confirmedDefinitionMeta = [:]
    confirmedLinks = [:]
    for d in s.definitions.compactMap({ $0 }) {
      if let l = d.linkJson { confirmedLinks[d.id] = try? JSONValue.parse(l) } else { confirmedDefinitions[d.id] = try? JSONValue.parse(d.cardJson) }
      if let m = d.metaJson { confirmedDefinitionMeta[d.id] = try? JSONValue.parse(m) }
    }
    sources = [:]
    for src in s.sources.compactMap({ $0 }) { sources[src.path] = (src.hash ?? "", src.sizeBytes ?? 0) }
    refreshExternals()
    applyResolution(s.resolution.compactMap { $0 }, s.diagnostics.compactMap { $0 })
    runStatus = s.run
    if let v = s.view {
      viewRev = s.viewRev ?? 0
      initialScope = v.scope.path
      for cam in v.cameras.compactMap({ $0 }) {
        cameras[key(cam.scope.path)] = (cam.x ?? 0, cam.y ?? 0, cam.zoom ?? 1)
      }
      initialSelection = Set(v.selectedCards.compactMap { $0.map { key($0.path) } })
    } else {
      viewRev = s.viewRev ?? 0
    }
    if resetDraft {
      project = confirmedProject
      layout = confirmedLayout
      sourceEdits = [:]
      sourceRemovals = []
      definitionEdits = [:]
      externalEdits = [:]
      draftBase = nil
      externalBases = [:]
    } else if draftBase?.rev != rev {
      // Edits kept over a reconnect: merged onto what is there now, then saved.
      rebase()
    }
  }

  private func applyResolution(_ res: [Tsugumi_RPC_ResolvedCardT], _ diags: [Tsugumi_Data_DiagnosticT]) {
    var r: [CardKey: ResolvedCard] = [:]
    var all = diags.map(Diagnostic.init)
    for rc in res {
      let p = rc.card.path
      let port = { (x: Tsugumi_Data_PortT) -> PortInfo in PortInfo(name: x.name, kind: x.kind, optional: x.optional_ ?? false, rootType: x.rootType) }
      let d = rc.diagnostics.compactMap { $0 }.map(Diagnostic.init)
      r[key(p)] = ResolvedCard(path: p, inputs: rc.inputs.compactMap { $0 }.map(port),
                               outputs: rc.outputs.compactMap { $0 }.map(port), diagnostics: d)
      all += d
    }
    resolution = r
    diagnostics = all
    // A result belongs to a Card of the document. A Card that comes back asks for
    // its own again: displayedCards changed (refreshVisibleDisplays).
    for k in displays.keys where r[k] == nil { displays[k] = nil }
  }

  private func applyDelta(_ d: Tsugumi_RPC_DocumentDeltaT, fromSave: Bool = false) {
    guard d.baseRev == rev else {
      if let r = d.rev, r <= rev { return }  // already applied (our own save reply)
      reconnect()
      return
    }
    let wasClean = !dirty
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
      confirmedLinks[def] = nil
    }
    for def in d.definitionsUpsert.compactMap({ $0 }) {
      if let l = def.linkJson {
        confirmedLinks[def.id] = try? JSONValue.parse(l)
        confirmedDefinitions[def.id] = nil
      } else {
        confirmedDefinitions[def.id] = try? JSONValue.parse(def.cardJson)
        confirmedLinks[def.id] = nil
      }
      confirmedDefinitionMeta[def.id] = def.metaJson.flatMap { try? JSONValue.parse($0) }
    }
    for s in d.sourcesUpsert.compactMap({ $0 }) { sources[s.path] = (s.hash ?? "", s.sizeBytes ?? 0) }
    for p in d.sourcesRemoved.compactMap({ $0 }) { sources[p] = nil }
    refreshExternals()
    applyResolution(d.resolution.compactMap { $0 }, d.diagnostics.compactMap { $0 })
    if let presenceBefore { notePresence(d, before: presenceBefore) }
    // A Draft implemented (or any Card that became code or a logger) sends its displays too.
    if displayedCards != shown { Task { await refreshVisibleDisplays() } }
    if wasClean {
      // Nothing local to keep: follow the confirmed document.
      project = confirmedProject
      layout = confirmedLayout
      draftBase = nil
    } else if !fromSave {
      // Changed elsewhere (the Agent, another window) while edits are on
      // their way: shown at once, the edits merged on top.
      rebase()
    }
  }

  private func applyRunEvent(_ e: Tsugumi_RPC_RunEventT) {
    guard let v = e.value else { return }
    switch v.type {
    case .runacceptedevent:
      if let s = (v.value as? Tsugumi_RPC_RunAcceptedEventT)?.status {
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
        let k = key(inv.card.path)
        let prev = cardStates[k]
        let n = inv.invocation ?? 0
        if prev == nil || prev!.invocation <= n {
          var next = CardRunState(state: inv.state, invocation: n, phase: inv.phase, failure: inv.failure?.diagnostic)
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
        sampleClock(inv.endedAt ?? (inv.state == .running ? inv.stages.last??.start : nil))
        recordTimeline(k, inv)
      }
    case .runfinished:
      if let s = (v.value as? Tsugumi_RPC_RunFinishedT)?.summary.status { runStatus = s }
      lastFinished = Date()
      if let commit = runStatus?.commit { Task { await summarizeVersion(commit) } }
      if dirty { scheduleSave() }
    default:
      break
    }
  }

  private func applyDisplay(_ d: Tsugumi_Data_DisplayT) {
    let k = key(d.card.path)
    let rev = d.revision ?? 0
    if let old = displays[k], old.runId == d.runId, old.revision >= rev { return }  // stale frame
    let value: DisplayValue
    switch d.value?.type {
    case .displaytext:
      let t = d.value?.value as? Tsugumi_Data_DisplayTextT
      value = t?.markdown == true ? .markdown(t?.text ?? "") : .text(t?.text ?? "")
    case .displayscalar:
      let s = d.value?.value as? Tsugumi_Data_DisplayScalarT
      value = .scalar(s?.value ?? 0, s?.label ?? "")
    case .displaylinechart:
      let c = d.value?.value as? Tsugumi_Data_DisplayLineChartT
      value = .lineChart((c?.series ?? []).compactMap { $0 }.map { ($0.id, $0.label, $0.x, $0.y) })
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
      value = .module(name: m?.name ?? "", json: m?.dataJson ?? "")
    default: return
    }
    displays[k] = DisplaySnapshot(runId: d.runId, invocation: d.invocation ?? 0, revision: rev, value: value)
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
      timeline.append(TimelineEntry(card: k, invocation: n, state: inv.state, startedAt: inv.startedAt, endedAt: inv.endedAt,
                                    stages: stages))
    }
  }

  // MARK: - Editing (draft, undo/redo)

  private func snapshot() -> Snapshot {
    Snapshot(project: project, layout: layout, sources: sourceEdits, removals: sourceRemovals, definitions: definitionEdits,
             externals: externalEdits)
  }

  /// Confirmed definitions as definition files (card, meta, link).
  var confirmedDefinitionDrafts: [String: DefinitionDraft] {
    var d: [String: DefinitionDraft] = [:]
    for (id, c) in confirmedDefinitions { d[id] = DefinitionDraft(card: c, meta: confirmedDefinitionMeta[id]) }
    for (id, l) in confirmedLinks { d[id] = DefinitionDraft(card: .object([]), meta: confirmedDefinitionMeta[id], link: l) }
    return d
  }

  /// Definitions with the draft applied, as definition files.
  var definitionDrafts: [String: DefinitionDraft] {
    var d = confirmedDefinitionDrafts
    for (id, e) in definitionEdits { d[id] = e.removed ? nil : e }
    return d
  }

  /// Externals with the draft applied.
  var effectiveExternals: [String: ExternalProject] {
    var d = externals
    for (p, e) in externalEdits { d[p] = e.removed ? nil : e }
    return d
  }

  func docState() -> DocState {
    DocState(project: project, layout: layout, definitions: definitionDrafts, externals: effectiveExternals)
  }

  private func makeBase() -> DraftBase {
    DraftBase(rev: rev, hashes: sources.mapValues(\.hash), project: confirmedProject, layout: confirmedLayout,
              definitions: confirmedDefinitionDrafts)
  }

  /// Fixes the draft's base when it leaves the clean state, and forgets it
  /// when the draft becomes clean again.
  private func settleDraftBase() {
    if !dirty {
      draftBase = nil
      externalBases = [:]
    } else if draftBase == nil {
      draftBase = makeBase()
    }
  }

  /// Sets a definition as it should look with the draft applied.
  func setEffectiveDefinition(_ id: String, _ d: DefinitionDraft?) {
    let confirmed = confirmedDefinitionDrafts[id]
    if d == confirmed { definitionEdits[id] = nil } else { definitionEdits[id] = d ?? DefinitionDraft(card: .object([]), meta: nil, removed: true) }
  }

  /// Changes an external's documents in the draft.
  func updateExternal(_ prefix: String, _ fn: (inout ExternalProject) -> Void) {
    guard !prefix.isEmpty, var e = externalEdits[prefix] ?? externals[prefix] else { return }
    if externalEdits[prefix] == nil, externalBases[prefix] == nil { externalBases[prefix] = externals[prefix] }
    fn(&e)
    externalEdits[prefix] = e
  }

  /// Adds a new external (its documents; other files go to the sources).
  func createExternal(_ prefix: String, _ e: ExternalProject) {
    if externalBases[prefix] == nil { externalBases[prefix] = .some(externals[prefix]) }
    externalEdits[prefix] = e
  }

  /// Removes an external with every file in its folder.
  func removeExternal(_ prefix: String) {
    guard var e = externalEdits[prefix] ?? externals[prefix] else { return }
    if externalBases[prefix] == nil { externalBases[prefix] = .some(externals[prefix]) }
    e.removed = true
    externalEdits[prefix] = e
    for p in sourcePaths where p.hasPrefix(prefix) { removeSource(p) }
  }

  /// Drops edits of externals that equal what is confirmed.
  private func normalizeExternalEdits() {
    for (p, e) in externalEdits where (e.removed && externals[p] == nil) || (!e.removed && e == externals[p]) {
      externalEdits[p] = nil
      externalBases[p] = nil
    }
  }

  private func sourceValue(_ snap: Snapshot, _ p: String) -> SourceValue {
    if snap.removals.contains(p) { return .absent }
    if let d = snap.sources[p] { return .bytes(d) }
    return sources[p] != nil ? .confirmed : .absent
  }

  @ObservationIgnored private var unfilled: [UndoEntry] = []

  private func record(before: DocState, beforeSnap: Snapshot, after: DocState, afterSnap: Snapshot) -> UndoEntry {
    let paths = Set(beforeSnap.sources.keys).union(afterSnap.sources.keys).union(beforeSnap.removals).union(afterSnap.removals)
    var sb: [String: SourceValue] = [:], sa: [String: SourceValue] = [:]
    for p in paths where sourceValue(beforeSnap, p) != sourceValue(afterSnap, p) {
      sb[p] = sourceValue(beforeSnap, p)
      sa[p] = sourceValue(afterSnap, p)
    }
    let e = UndoEntry(before: before, after: after, sourcesBefore: sb, sourcesAfter: sa)
    if sb.values.contains(.confirmed) { unfilled.append(e) }
    return e
  }

  /// Applies an edit and saves it at once. One call is one step of undo.
  func edit(_ change: (ProjectSession) -> Void) {
    let before = docState(), beforeSnap = snapshot()
    if !dirty { draftBase = makeBase() }
    change(self)
    normalizeExternalEdits()
    let afterSnap = snapshot()
    guard beforeSnap != afterSnap else {
      settleDraftBase()
      return
    }
    undoStack.append(record(before: before, beforeSnap: beforeSnap, after: docState(), afterSnap: afterSnap))
    if undoStack.count > 500 { undoStack.removeFirst() }
    redoStack.removeAll()
    settleDraftBase()
    scheduleSave()
  }

  func undo() {
    guard let e = undoStack.popLast() else { return }
    if let inverse = revert(e) { redoStack.append(inverse) }
  }

  func redo() {
    guard let e = redoStack.popLast() else { return }
    if let inverse = revert(e) { undoStack.append(inverse) }
  }

  /// Puts back what an entry changed (its `before`), over the current state.
  /// Returns the entry that does it again.
  private func revert(_ e: UndoEntry) -> UndoEntry? {
    let cur = docState(), curSnap = snapshot()
    if !dirty { draftBase = makeBase() }
    project = merge3(base: e.after.project, ours: e.before.project, theirs: cur.project) ?? cur.project
    layout = merge3(base: e.after.layout, ours: e.before.layout, theirs: cur.layout) ?? cur.layout
    for id in Set(e.before.definitions.keys).union(e.after.definitions.keys) where e.before.definitions[id] != e.after.definitions[id] {
      setEffectiveDefinition(id, DefinitionDraft.merge(base: e.after.definitions[id], ours: e.before.definitions[id],
                                                       theirs: cur.definitions[id]))
    }
    for p in Set(e.before.externals.keys).union(e.after.externals.keys) where e.before.externals[p] != e.after.externals[p] {
      if let b = e.before.externals[p] {
        if externalBases[p] == nil { externalBases[p] = .some(externals[p]) }
        var t = ExternalProject.merge(base: e.after.externals[p], ours: b, theirs: cur.externals[p])
        t.removed = false
        externalEdits[p] = t
      } else {
        removeExternal(p)
      }
    }
    for (p, v) in e.sourcesBefore {
      switch v {
      case .absent: removeSource(p)
      case .bytes(let d):
        sourceRemovals.remove(p)
        sourceEdits[p] = d
      case .confirmed: break  // could not be read: left as it is
      }
    }
    normalizeExternalEdits()
    settleDraftBase()
    let now = snapshot()
    guard now != curSnap else { return nil }
    var sb: [String: SourceValue] = [:]
    for p in e.sourcesBefore.keys { sb[p] = sourceValue(curSnap, p) }
    let inverse = UndoEntry(before: cur, after: docState(), sourcesBefore: sb, sourcesAfter: e.sourcesBefore)
    if sb.values.contains(.confirmed) { unfilled.append(inverse) }
    scheduleSave()
    return inverse
  }

  /// Reads the files undo entries need before a save replaces them.
  private func fillUndoSources() async {
    guard let client, !unfilled.isEmpty else { return }
    let list = unfilled
    unfilled = []
    for e in list {
      for (p, v) in e.sourcesBefore where v == .confirmed {
        if let d = try? await client.readFile(path: p, atRev: rev) { e.sourcesBefore[p] = .bytes(d) }
      }
    }
  }

  /// Moves the draft onto the confirmed state that just arrived: what the
  /// draft changed stays, everything else follows the new state.
  private func rebase() {
    guard let b = draftBase else { return }
    project = merge3(base: b.project, ours: project, theirs: confirmedProject) ?? confirmedProject
    layout = merge3(base: b.layout, ours: layout, theirs: confirmedLayout) ?? confirmedLayout
    let theirs = confirmedDefinitionDrafts
    for (id, e) in definitionEdits {
      setEffectiveDefinition(id, DefinitionDraft.merge(base: b.definitions[id], ours: e.removed ? nil : e, theirs: theirs[id]))
    }
    var hashes = sources.mapValues(\.hash)
    // Files edited here stay checked against the version they were edited from.
    for p in Set(sourceEdits.keys).union(sourceRemovals) { hashes[p] = b.hashes[p] }
    draftBase = DraftBase(rev: rev, hashes: hashes, project: confirmedProject, layout: confirmedLayout, definitions: theirs)
    settleDraftBase()
  }

  /// Drops every unsaved edit and follows the latest confirmed document
  /// (only for edits that cannot be saved).
  func discardDraft() {
    dropDraft()
    // A save on its way answers about the draft that went: nothing of it stays.
    if let saving = saveTask {
      Task { [weak self] in
        await saving.value
        self?.dropDraft()
      }
    }
  }

  private func dropDraft() {
    project = confirmedProject
    layout = confirmedLayout
    sourceEdits = [:]
    sourceRemovals = []
    definitionEdits = [:]
    externalEdits = [:]
    sourceBases = [:]
    draftBase = nil
    externalBases = [:]
    pendingCardRenames = []
    saveAgain = false
    conflictRetries = 0
    saveError = nil
    savingAfterRun = false
  }

  /// Starts saving what the draft holds (edits made meanwhile are saved next).
  func scheduleSave() {
    guard connected, client != nil else { return }
    if saveTask != nil {
      saveAgain = true
      return
    }
    saveTask = Task { [weak self] in
      guard let self else { return }
      var attempts = 0
      repeat {
        self.saveAgain = false
        attempts += 1
        await self.saveOnce()
      } while self.saveAgain && attempts < 50
      self.saveTask = nil
    }
  }

  /// Waits until what was edited so far is saved (or cannot be saved now).
  func save() async {
    if saveTask == nil { scheduleSave() } else { saveAgain = true }
    while let t = saveTask { await t.value }
  }

  @ObservationIgnored private var conflictRetries = 0

  /// Sends the draft as one ProjectSave transaction. Changes that arrived
  /// meanwhile are merged in first; a conflict means newer changes are on
  /// their way, so the save is tried again once they arrived.
  private func saveOnce() async {
    guard let client else { return }
    // Nothing left to save (undone, or met by what arrived): no failure is left either.
    guard dirty else {
      saveError = nil
      return
    }
    await fillUndoSources()
    if let b = draftBase, b.rev != rev { rebase() }
    guard dirty else {
      saveError = nil
      return
    }
    saving = true
    defer { saving = false }
    let base = draftBase ?? makeBase()
    // While a run is active the core takes layout changes only.
    let layoutOnly = runActive && computationDirty
    savingAfterRun = layoutOnly
    var changes: [Tsugumi_RPC_DocumentChangeT] = []
    let sent = snapshot()
    if layout != confirmedLayout { changes.append(Change.setLayout(layout.serialized())) }
    var merged: [String: ExternalProject] = [:]
    if !layoutOnly {
      if project != confirmedProject { changes.append(Change.setProject(project.serialized())) }
      // A removed definition takes its folder with it: the core refuses a change to a file in it.
      var removedFolders: [String] = []
      for (id, d) in definitionEdits.sorted(by: { $0.key < $1.key }) {
        if d.removed {
          if confirmedDefinitions[id] != nil || confirmedLinks[id] != nil {
            changes.append(Change.removeDefinition(id: id))
            removedFolders.append("definitions/\(id)/")
          }
        } else {
          changes.append(Change.putDefinition(id: id, cardJSON: d.card.serialized(), metaJSON: d.meta?.serialized(),
                                              linkJSON: d.link?.serialized()))
        }
      }
      var written = Set<String>()
      for (prefix, ours) in externalEdits.sorted(by: { $0.key < $1.key }) where !ours.removed {
        let theirs = externals[prefix]
        let m = ExternalProject.merge(base: externalBases[prefix] ?? nil, ours: ours, theirs: theirs)
        merged[prefix] = m
        let now = m.files, was = theirs?.files ?? [:]
        for (rel, text) in now.sorted(by: { $0.key < $1.key }) where was[rel] != text {
          let p = prefix + rel
          written.insert(p)
          changes.append(Change.putSource(path: p, bytes: Data(text.utf8), baseHash: theirs == nil ? sources[p]?.hash : externalHashes[p]))
        }
        for rel in was.keys.sorted() where now[rel] == nil {
          let p = prefix + rel
          if let h = externalHashes[p] ?? sources[p]?.hash, !sourceRemovals.contains(p) {
            written.insert(p)
            changes.append(Change.removeSource(path: p, baseHash: h))
          }
        }
      }
      func goesWithItsDefinition(_ p: String) -> Bool { removedFolders.contains { p.hasPrefix($0) } }
      for (p, bytes) in sourceEdits.sorted(by: { $0.key < $1.key })
      where !sourceRemovals.contains(p) && !written.contains(p) && !goesWithItsDefinition(p) {
        changes.append(Change.putSource(path: p, bytes: bytes, baseHash: sourceBases[p] ?? base.hashes[p]))
      }
      for p in sourceRemovals.sorted() where !written.contains(p) && !goesWithItsDefinition(p) {
        if let h = sourceBases[p] ?? base.hashes[p] ?? sources[p]?.hash { changes.append(Change.removeSource(path: p, baseHash: h)) }
      }
    }
    guard !changes.isEmpty else {
      if !layoutOnly { forgetSaved(sent, layoutOnly: false, merged: [:]) }
      return
    }
    let op = UUID()
    do {
      let renames = layoutOnly ? [] : pendingCardRenames
      let result = try await client.save(baseRev: base.rev, opId: op, origin: "ui:\(viewId.uuidString.lowercased())", changes: changes,
                                         renamed: renames)
      if !renames.isEmpty { pendingCardRenames.removeFirst(min(renames.count, pendingCardRenames.count)) }
      saveError = nil
      conflictRetries = 0
      if let d = result.delta { applyDeltaFromSave(d) }
      forgetSaved(sent, layoutOnly: layoutOnly, merged: merged)
    } catch let f as RPCFailure {
      switch f.kind {
      case .revisionConflict, .hashConflict:
        if f.kind == .hashConflict, let p = f.args["path"], !isExternalDocument(p) {
          // A file this window edited was changed elsewhere too: kept, not merged.
          saveError = String(localized: "\(p) は別の場所でも変更されたため保存していません。変更は保持しています。")
          return
        }
        guard conflictRetries < 8 else {
          conflictRetries = 0
          saveError = String(localized: "他の変更と重なり保存できませんでした。変更は保持しています。")
          return
        }
        // Newer changes are on their way (DocPatched): merge them in and try again.
        conflictRetries += 1
        let want = f.currentRev ?? rev + 1
        for _ in 0..<40 where rev < want { try? await Task.sleep(for: .milliseconds(50)) }
        try? await Task.sleep(for: .milliseconds(30 * conflictRetries))
        saveAgain = true
      case .busy:
        savingAfterRun = true
      case .externalChange:
        saveError = String(localized: "\(f.args["path"] ?? "ファイル") が Tsugumi の外で変更されました。再読込で取り込めます（変更は保持しています）。")
      default:
        saveError = f.description
      }
    } catch {
      // The reply may have been lost after the save committed: ask the core.
      if let r = try? await client.operation(op), r.state == .completed {
        saveError = nil
        forgetSaved(sent, layoutOnly: layoutOnly, merged: merged)
      } else {
        saveError = String(localized: "保存できませんでした: \(String(describing: error))。変更は保持しています。")
      }
    }
  }

  /// Whether a path is one of an external's documents (merged, not compared by hash).
  private func isExternalDocument(_ p: String) -> Bool {
    effectiveExternals.keys.contains { p.hasPrefix($0) && ExternalProject.isDocument(String(p.dropFirst($0.count))) }
  }

  /// After a save: the parts sent are confirmed now; edits made while it was
  /// on its way stay in the draft.
  private func forgetSaved(_ sent: Snapshot, layoutOnly: Bool, merged: [String: ExternalProject]) {
    if layout == sent.layout { layout = confirmedLayout } else { layout = merge3(base: sent.layout, ours: layout, theirs: confirmedLayout) ?? layout }
    if !layoutOnly {
      if project == sent.project { project = confirmedProject } else { project = merge3(base: sent.project, ours: project, theirs: confirmedProject) ?? project }
      for (id, d) in sent.definitions where definitionEdits[id] == d { definitionEdits[id] = nil }
      for (p, d) in sent.sources where sourceEdits[p] == d {
        sourceEdits[p] = nil
        sourceBases[p] = nil
      }
      for p in sent.removals where sourceRemovals.contains(p) && sent.sources[p] == nil {
        sourceRemovals.remove(p)
        sourceBases[p] = nil
      }
      for (prefix, m) in merged {
        // What was written is now the external's confirmed content.
        externals[prefix] = m
        for rel in m.files.keys { externalHashes[prefix + rel] = sources[prefix + rel]?.hash }
        if externalEdits[prefix] == sent.externals[prefix] {
          externalEdits[prefix] = nil
          externalBases[prefix] = nil
        } else {
          externalBases[prefix] = .some(m)
        }
      }
      for (prefix, e) in sent.externals where e.removed && externalEdits[prefix] == e {
        externalEdits[prefix] = nil
        externalBases[prefix] = nil
        externals[prefix] = nil
      }
      savingAfterRun = false
    }
    // Whatever is left was edited on top of what was just saved.
    draftBase = nil
    settleDraftBase()
  }

  private func applyDeltaFromSave(_ d: Tsugumi_RPC_DocumentDeltaT) {
    if d.baseRev == rev {
      let keep = snapshot()
      applyDelta(d, fromSave: true)
      project = keep.project
      layout = keep.layout
    }
  }

  func reloadExternal() async {
    guard let client else { return }
    do {
      _ = try await client.reload(baseRev: rev, opId: UUID(), origin: "ui")
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
      let v = Tsugumi_RPC_ViewStateT()
      v.scope = cardPath(scope)
      v.cameras = cameras.map { k, c in
        let cam = Tsugumi_RPC_CameraT()
        cam.scope = cardPath(path(k))
        cam.x = c.x
        cam.y = c.y
        cam.zoom = c.zoom
        return cam
      }
      v.selectedCards = selection.sorted().map { cardPath(path($0)) }
      do {
        let r = try await client.saveView(viewId: viewId, baseRev: viewRev, view: v)
        viewRev = r.viewRev ?? viewRev
      } catch let f as RPCFailure where f.kind == .revisionConflict {
        viewRev = f.currentRev ?? viewRev
      } catch {}
    }
  }

  /// Cards whose displays the canvas shows: what a Card shows is up to its
  /// code (the display SDK), so every Card that can run may send displays.
  var displayedCards: Set<CardKey> { Set(resolution.keys.filter { [.code, .logger].contains(kind($0)) }) }

  /// A Card that took a name another Card had starts without that Card's result and run state.
  func forgetResult(_ k: CardKey) {
    displays[k] = nil
    cardStates[k] = nil
  }

  func refreshVisibleDisplays() async {
    guard let client, let subscription else { return }
    try? await client.setVisible(subscription: subscription, cards: displayedCards.sorted().map(path), enabled: true)
  }

  // MARK: - Runs

  func run(card: [String], propagate: Bool) async throws {
    guard let client else { throw RPCError.disconnected }
    if computationDirty { await save() }
    if computationDirty {
      throw RPCFailure(kind: .validationFailed, diagnostic: saveError ?? String(localized: "変更を保存できないため実行できません。"))
    }
    let op = UUID()
    let parallel = UInt32(max(1, min(64, AppPreferences.shared.maxConcurrency)))
    do {
      let accepted = try await client.run(baseRev: rev, opId: op, card: card, propagate: propagate, maxConcurrency: parallel)
      runStatus = accepted.run
      cardStates = [:]
      timeline = []
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

  /// Duplicates the confirmed document of one revision into a new project:
  /// documents and sources read over RPC, revision re-checked after the last
  /// read. History, caches and unsaved edits are not copied; the copy gets a
  /// new project_id when its environment starts.
  func duplicate(to dest: URL) async throws {
    guard let client else { throw RPCError.disconnected }
    let snap = try await client.inspect()
    let fm = FileManager.default
    guard !fm.fileExists(atPath: dest.path) else { throw CocoaError(.fileWriteFileExists) }
    do {
      try fm.createDirectory(at: dest, withIntermediateDirectories: true)
      try Data(snap.projectJson.utf8).write(to: dest.appendingPathComponent("tsugumi.json"))
      try Data(snap.layoutJson.utf8).write(to: dest.appendingPathComponent("layout.json"))
      for d in snap.definitions.compactMap({ $0 }) {
        let dir = dest.appendingPathComponent("definitions/\(d.id)", isDirectory: true)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        if let l = d.linkJson {
          try Data(l.utf8).write(to: dir.appendingPathComponent("_link.json"))
        } else {
          try Data(d.cardJson.utf8).write(to: dir.appendingPathComponent("_card.json"))
        }
        if let m = d.metaJson { try Data(m.utf8).write(to: dir.appendingPathComponent("_meta.json")) }
      }
      for src in snap.sources.compactMap({ $0 }) {
        let data = try await client.readFile(path: src.path, atRev: snap.rev ?? 0)
        let file = dest.appendingPathComponent(src.path)
        try fm.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: file)
      }
      let again = try await client.inspect()
      guard again.rev == snap.rev else {
        throw RPCFailure(kind: .revisionConflict, diagnostic: "The project changed while it was copied; try again.")
      }
    } catch {
      try? fm.removeItem(at: dest)
      throw error
    }
  }

  // MARK: - Document helpers

  /// The project a scope's Cards are written in: "" for this project,
  /// "externals/<name>/" (nested further) inside a Card in Card; nil when the
  /// scope is not a Card in Card.
  func projectPrefix(_ scope: [String]) -> String? {
    var prefix = ""
    for id in scope {
      let link: String?
      if prefix.isEmpty { link = links[id]?["external"]?.string } else { link = external(prefix)?.links[id] }
      guard let name = link else { return nil }
      prefix += "externals/\(name)/"
    }
    return prefix
  }

  /// An external's documents with the draft applied.
  func external(_ prefix: String) -> ExternalProject? {
    if let e = externalEdits[prefix] { return e.removed ? nil : e }
    return externals[prefix]
  }

  /// Whether a scope is inside a Card in Card (its Cards belong to the external's copy).
  func isExternalScope(_ scope: [String]) -> Bool { !scope.isEmpty }

  private func graph(_ scope: [String]) -> JSONValue? {
    guard let prefix = projectPrefix(scope) else { return nil }
    return prefix.isEmpty ? project["canvas"] : external(prefix)?.project["canvas"]
  }

  /// Cards of a scope: this project's, or (inside a Card in Card) the external's.
  func cards(in scope: [String]) -> [JSONValue] { graph(scope)?["cards"]?.array ?? [] }

  func connections(in scope: [String]) -> [JSONValue] { graph(scope)?["connections"]?.array ?? [] }

  /// Mutates a scope's canvas in the draft (inside a Card in Card: its external's).
  func updateGraph(_ scope: [String], _ fn: (inout JSONValue) -> Void) {
    guard let prefix = projectPrefix(scope) else { return }
    if prefix.isEmpty {
      var canvas = project["canvas"] ?? .object([("cards", .array([])), ("connections", .array([]))])
      fn(&canvas)
      project["canvas"] = canvas
    } else {
      updateExternal(prefix) { e in
        var canvas = e.project["canvas"] ?? .object([("cards", .array([])), ("connections", .array([]))])
        fn(&canvas)
        e.project["canvas"] = canvas
      }
    }
  }

  /// The layout a scope's Cards, notes and groups are kept in, and the key of
  /// a Card there (inside a Card in Card: the external's layout, by Card id).
  func layout(of scope: [String]) -> JSONValue {
    guard let prefix = projectPrefix(scope), !prefix.isEmpty else { return layout }
    return external(prefix)?.layout ?? .object([])
  }

  func updateLayout(of scope: [String], _ fn: (inout JSONValue) -> Void) {
    guard let prefix = projectPrefix(scope) else { return }
    if prefix.isEmpty { fn(&layout) } else { updateExternal(prefix) { fn(&$0.layout) } }
  }

  /// A Card's key in the layout of its scope.
  func layoutKey(_ k: CardKey) -> String { path(k).count > 1 ? (path(k).last ?? k) : k }

  /// The scope a note's or group's "scope" field means in the layout it is in.
  func layoutScopeKey(_ scope: [String]) -> String { scope.count > 0 ? "" : key(scope) }

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
    guard let d = cardDefinitionDraft(k), d.link == nil else { return nil }
    return d.card
  }

  /// The Card's _meta.json.
  func cardMeta(_ k: CardKey) -> JSONValue? { cardDefinitionDraft(k)?.meta }

  /// The confirmed definition files of a Card (not the draft).
  func confirmedCardDefinitionDraft(_ k: CardKey) -> DefinitionDraft? {
    let p = path(k)
    guard let id = p.last, let prefix = projectPrefix(Array(p.dropLast())) else { return nil }
    return prefix.isEmpty ? confirmedDefinitionDrafts[id] : externals[prefix]?.definitions[id]
  }

  /// Sets a Card's definition files in the draft (nil removes them).
  func setCardDefinition(_ k: CardKey, _ d: DefinitionDraft?) {
    let p = path(k)
    guard let id = p.last, let prefix = projectPrefix(Array(p.dropLast())) else { return }
    if prefix.isEmpty { setEffectiveDefinition(id, d) } else { updateExternal(prefix) { $0.definitions[id] = d } }
  }

  /// The external project of a Card in Card ("externals/<name>/…"), if it is one.
  func cardExternal(_ k: CardKey) -> String? {
    let p = path(k)
    guard let id = p.last, let prefix = projectPrefix(Array(p.dropLast())) else { return nil }
    let name = prefix.isEmpty ? links[id]?["external"]?.string : external(prefix)?.links[id]
    return name.map { prefix + "externals/\($0)/" }
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
    guard var d = cardDefinitionDraft(k), d.link == nil else { return }
    fn(&d.card)
    setCardDefinition(k, d)
  }

  /// Changes a Card's _meta.json in the draft.
  func updateMeta(_ k: CardKey, _ fn: (inout JSONValue) -> Void) {
    guard var d = cardDefinitionDraft(k), d.link == nil else { return }
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

  /// The Cards of a scope with comments not resolved yet.
  func cardsWithOpenComments(in scope: [String]) -> [CardKey] {
    cards(in: scope).compactMap { $0["id"]?.string }.map { key(scope + [$0]) }.filter { !openComments($0).isEmpty }
  }

  /// Saved at once; a Card in Card has a _meta.json too. One call is one step of undo.
  private func editComments(_ k: CardKey, _ fn: ([JSONValue]) -> [JSONValue]) {
    edit { x in
      guard var d = x.cardDefinitionDraft(k) else { return }
      var meta = d.meta ?? .object([])
      let list = fn(meta["comments"]?.array ?? [])
      meta["comments"] = list.isEmpty ? nil : .array(list)
      d.meta = meta.object?.isEmpty ?? true ? nil : meta
      x.setCardDefinition(k, d)
    }
  }

  func addComment(_ k: CardKey, _ text: String) {
    let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !t.isEmpty else { return }
    editComments(k) { $0 + [.object([("id", .string(UUID().uuidString.lowercased())), ("text", .string(t)), ("author", .string("user"))])] }
  }

  func editComment(_ k: CardKey, id: String, text: String) {
    let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !t.isEmpty else { return }
    editComments(k) { list in
      list.map { c in
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

  func position(_ k: CardKey) -> CGPoint {
    let p = layout(of: Array(path(k).dropLast()))["cards"]?[layoutKey(k)]
    return CGPoint(x: p?["x"]?.double ?? 0, y: p?["y"]?.double ?? 0)
  }

  /// Reads the externals' documents when their sources changed.
  func refreshExternals() {
    let paths = sources.keys.filter { $0.hasPrefix("externals/") }.sorted()
    let key = paths.map { "\($0)=\(sources[$0]?.hash ?? "")" }.joined(separator: ";")
    guard key != externalsKey else { return }
    externalsKey = key
    guard let client else { return }
    let atRev = rev
    let hashes = sources.mapValues(\.hash)
    Task { @MainActor in
      var found: [String: ExternalProject] = [:]
      var read: [String: String] = [:]
      for p in paths where p.hasSuffix("/tsugumi.json") {
        let prefix = String(p.dropLast("tsugumi.json".count))
        // Only folders that are externals of a project: …externals/<name>/tsugumi.json.
        let parts = prefix.split(separator: "/")
        guard parts.count >= 2, parts.count % 2 == 0, parts[parts.count - 2] == "externals" else { continue }
        func doc(_ rel: String) async -> JSONValue? {
          guard let h = hashes[rel], let d = try? await client.readFile(path: rel, atRev: atRev) else { return nil }
          read[rel] = h
          return try? JSONValue.parse(String(decoding: d, as: UTF8.self))
        }
        guard let proj = await doc(p) else { continue }
        var ext = ExternalProject(project: proj, layout: await doc(prefix + "layout.json") ?? .object([]), definitions: [:])
        // Every definition folder of the external (placed or not).
        var ids = Set<String>()
        for q in paths where q.hasPrefix(prefix + "definitions/") {
          let rest = q.dropFirst((prefix + "definitions/").count).split(separator: "/")
          if rest.count == 2, ["_card.json", "_link.json"].contains(rest[1]) { ids.insert(String(rest[0])) }
        }
        for id in ids.sorted() {
          let dir = prefix + "definitions/\(id)/"
          let meta = await doc(dir + "_meta.json")
          if let l = await doc(dir + "_link.json") {
            ext.definitions[id] = DefinitionDraft(card: .object([]), meta: meta, link: l)
          } else if let card = await doc(dir + "_card.json") {
            ext.definitions[id] = DefinitionDraft(card: card, meta: meta)
          }
        }
        found[prefix] = ext
      }
      guard self.externalsKey == key else { return }
      self.externals = found
      self.externalHashes = read
      self.normalizeExternalEdits()
      self.settleDraftBase()
      // Cards inside Cards in Card are known now: their displays are wanted too.
      await self.refreshVisibleDisplays()
    }
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
