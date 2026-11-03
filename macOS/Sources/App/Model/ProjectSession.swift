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
/// canvas of a Card in Card. Read from the confirmed sources; edited only
/// where it opens on its own.
struct ExternalProject: Equatable {
  var project: JSONValue
  var layout: JSONValue
  var definitions: [String: JSONValue]  // Card id -> _card.json
  var links: [String: String]           // Card id -> external name (its own Cards in Card)
}

/// Save/sync state, shown separately from the run state.
enum SyncState: Equatable {
  case saved
  case unsaved
  case saving
  case failed(String)
  case offline
}

enum DisplayValue {
  case text(String)
  case scalar(Double, String)
  case lineChart([(id: String, label: String, x: [Double], y: [Double])])
  case table(columns: [String], rows: [[String]])
  case image(mediaType: String, data: Data, alt: String)
  case points(x: [Float], y: [Float], z: [Float], colors: [UInt32])
  case module(name: String, json: String)
}

struct DisplaySnapshot {
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
  private(set) var connectionMessage: String?
  @ObservationIgnored private var client: RPCClient?
  @ObservationIgnored private var subscription: String?
  @ObservationIgnored private var lastSeq: UInt64 = 0
  @ObservationIgnored private var listenTask: Task<Void, Never>?
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
  /// Externals by folder ("externals/<name>/", nested "…/externals/<name>/").
  private(set) var externals: [String: ExternalProject] = [:]
  @ObservationIgnored private var externalsKey = ""
  private(set) var sources: [String: (hash: String, size: UInt64)] = [:]
  private(set) var resolution: [CardKey: ResolvedCard] = [:]
  private(set) var diagnostics: [Diagnostic] = []

  // Draft (uncommitted, managed here)
  var project: JSONValue = .object([])
  var layout: JSONValue = .object([])
  var sourceEdits: [String: Data] = [:]  // path -> new bytes
  var sourceRemovals: Set<String> = []
  var definitionEdits: [String: DefinitionDraft] = [:]
  /// Base hashes chosen by an editor that read the file before this draft
  /// started: the save is checked against what the editor actually saw.
  var sourceBases: [String: String] = [:]
  private(set) var undoStack: [Snapshot] = []
  private(set) var redoStack: [Snapshot] = []
  private(set) var saveError: String?
  private(set) var remoteChangePending = false
  private(set) var saving = false
  /// Last user-facing problem of an action (run refused, import failed, ...).
  var notice: String?

  struct Snapshot: Equatable {
    var project: JSONValue
    var layout: JSONValue
    var sources: [String: Data]
    var removals: Set<String>
    var definitions: [String: DefinitionDraft]
  }

  /// The confirmed state the current draft was started from. While the draft
  /// is dirty, remote saves move `rev` and the source hashes forward, but a
  /// save must still be checked against the base the edits were made on;
  /// otherwise an edit of an old version would pass the conflict check and
  /// overwrite the newer change.
  private struct DraftBase {
    let rev: UInt64
    let hashes: [String: String]
  }
  @ObservationIgnored private var draftBase: DraftBase?
  /// Revision the draft is based on (equals `rev` while clean).
  var draftBaseRev: UInt64 { draftBase?.rev ?? rev }

  var dirty: Bool {
    project != confirmedProject || layout != confirmedLayout || !sourceEdits.isEmpty || !sourceRemovals.isEmpty ||
      !definitionEdits.isEmpty
  }
  var computationDirty: Bool {
    project != confirmedProject || !sourceEdits.isEmpty || !sourceRemovals.isEmpty || !definitionEdits.isEmpty
  }

  var syncState: SyncState {
    if !connected { return dirty ? .failed(connectionMessage ?? "Not connected") : .offline }
    if saving { return .saving }
    if let e = saveError { return .failed(e) }
    return dirty ? .unsaved : .saved
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
    Task { [weak self] in
      guard let self else { return }
      if observerToken == nil {
        observerToken = await environments.observe(identity) { [weak self] s in
          Task { @MainActor in self?.environment = s }
        }
      }
      await connect()
    }
  }

  private func connect() async {
    connectionMessage = nil
    do {
      let ep = try await environments.start(identity)
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
      await refreshVisibleDisplays()
    } catch {
      connected = false
      connectionMessage = "\(error)"
    }
  }

  /// Reconnects after a lost connection: Hello -> Open -> re-read state.
  /// Uncommitted edits are kept and never saved automatically.
  func reconnect() {
    environment = .reconnecting
    listenTask?.cancel()
    Task {
      await client?.close()
      client = nil
      connected = false
      for attempt in 0..<20 {
        await connect()
        if connected { return }
        try? await Task.sleep(for: .milliseconds(min(5000, 250 * (attempt + 1))))
      }
    }
  }

  /// Starts the environment again (folders shared with it are set when it
  /// starts); unsaved edits stay in the draft.
  func restartEnvironment() async {
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
        for event in batch { await self.handle(event) }
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

  private func handle(_ e: NotificationEvent) async {
    notificationCount += 1
    guard e.subscriptionId == subscription else { return }  // stale subscription
    if e.seq != lastSeq + 1 {
      // A gap means we cannot trust our state: open again.
      reconnect()
      return
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
      draftBase = nil
      remoteChangePending = false
    } else if draftBase?.rev != rev {
      remoteChangePending = true
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
  }

  private func applyDelta(_ d: Tsugumi_RPC_DocumentDeltaT) {
    guard d.baseRev == rev else {
      if let r = d.rev, r <= rev { return }  // already applied (our own save reply)
      reconnect()
      return
    }
    let wasClean = !dirty
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
    if wasClean {
      // Nothing local to keep: follow the confirmed document.
      project = confirmedProject
      layout = confirmedLayout
      draftBase = nil
    } else if d.origin != "ui:\(viewId.uuidString.lowercased())" {
      // The draft stays based on draftBase; the next save is checked against it.
      remoteChangePending = true
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
    case .displaytext: value = .text((d.value?.value as? Tsugumi_Data_DisplayTextT)?.text ?? "")
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
    Snapshot(project: project, layout: layout, sources: sourceEdits, removals: sourceRemovals, definitions: definitionEdits)
  }

  private func restore(_ s: Snapshot) {
    project = s.project
    layout = s.layout
    sourceEdits = s.sources
    sourceRemovals = s.removals
    definitionEdits = s.definitions
    settleDraftBase()
  }

  /// Fixes the draft's base when it leaves the clean state, and forgets it
  /// when the draft becomes clean again.
  private func settleDraftBase() {
    if !dirty {
      draftBase = nil
      remoteChangePending = false
    } else if draftBase == nil {
      draftBase = DraftBase(rev: rev, hashes: sources.mapValues(\.hash))
    }
  }

  /// Applies an edit to the draft and records it for undo.
  func edit(_ change: (ProjectSession) -> Void) {
    let before = snapshot()
    if !dirty { draftBase = DraftBase(rev: rev, hashes: sources.mapValues(\.hash)) }
    change(self)
    let after = snapshot()
    guard before != after else {
      settleDraftBase()
      return
    }
    undoStack.append(before)
    if undoStack.count > 500 { undoStack.removeFirst() }
    redoStack.removeAll()
    settleDraftBase()
  }

  func undo() {
    guard let s = undoStack.popLast() else { return }
    redoStack.append(snapshot())
    restore(s)
  }

  func redo() {
    guard let s = redoStack.popLast() else { return }
    undoStack.append(snapshot())
    restore(s)
  }

  /// Drops every unsaved edit and follows the latest confirmed document.
  func discardDraft() {
    edit { s in
      s.project = s.confirmedProject
      s.layout = s.confirmedLayout
      s.sourceEdits = [:]
      s.sourceRemovals = []
      s.definitionEdits = [:]
    }
    sourceBases = [:]
    draftBase = nil
    remoteChangePending = false
    saveError = nil
  }

  /// The unsaved edits as text, for copying them out when they cannot be saved.
  func draftSummary() -> String {
    var out = "Tsugumi unsaved edits (based on revision \(draftBaseRev))\n"
    if project != confirmedProject { out += "\n== tsugumi.json ==\n" + project.serialized() + "\n" }
    if layout != confirmedLayout { out += "\n== layout.json ==\n" + layout.serialized() + "\n" }
    for (id, d) in definitionEdits.sorted(by: { $0.key < $1.key }) {
      if d.removed { out += "\n== definitions/\(id) removed ==\n"; continue }
      out += "\n== definitions/\(id)/_card.json ==\n" + d.card.serialized() + "\n"
      if let m = d.meta { out += "\n== definitions/\(id)/_meta.json ==\n" + m.serialized() + "\n" }
    }
    for (p, bytes) in sourceEdits.sorted(by: { $0.key < $1.key }) {
      out += "\n== \(p) ==\n" + String(decoding: bytes, as: UTF8.self) + "\n"
    }
    for p in sourceRemovals.sorted() { out += "\n== \(p) removed ==\n" }
    return out
  }

  /// Commits the draft as one ProjectSave transaction. On conflict the draft is
  /// kept and the conflict is shown; nothing is rebased automatically.
  func save() async {
    guard dirty, let client else { return }
    saving = true
    defer { saving = false }
    // Checked against the base the edits were made on, never the latest rev.
    let base = draftBase ?? DraftBase(rev: rev, hashes: sources.mapValues(\.hash))
    var changes: [Tsugumi_RPC_DocumentChangeT] = []
    if project != confirmedProject { changes.append(Change.setProject(project.serialized())) }
    if layout != confirmedLayout { changes.append(Change.setLayout(layout.serialized())) }
    for (id, d) in definitionEdits.sorted(by: { $0.key < $1.key }) {
      if d.removed {
        if confirmedDefinitions[id] != nil { changes.append(Change.removeDefinition(id: id)) }
      } else {
        changes.append(Change.putDefinition(id: id, cardJSON: d.card.serialized(), metaJSON: d.meta?.serialized(),
                                            linkJSON: d.link?.serialized()))
      }
    }
    for (p, bytes) in sourceEdits.sorted(by: { $0.key < $1.key }) where !sourceRemovals.contains(p) {
      changes.append(Change.putSource(path: p, bytes: bytes, baseHash: sourceBases[p] ?? base.hashes[p]))
    }
    for p in sourceRemovals.sorted() {
      if let h = sourceBases[p] ?? base.hashes[p] { changes.append(Change.removeSource(path: p, baseHash: h)) }
    }
    let op = UUID()
    let saved = snapshot()
    do {
      let result = try await client.save(baseRev: base.rev, opId: op, origin: "ui:\(viewId.uuidString.lowercased())", changes: changes)
      saveError = nil
      if let d = result.delta {
        applyDeltaFromSave(d)
      }
      // Edits made while the save was in flight stay in the draft.
      if snapshot() == saved {
        sourceEdits = [:]
        sourceRemovals = []
        definitionEdits = [:]
        sourceBases = [:]
        project = confirmedProject
        layout = confirmedLayout
      }
      // Whatever is left was edited on top of what was just saved.
      draftBase = nil
      settleDraftBase()
      remoteChangePending = false
    } catch let f as RPCFailure {
      switch f.kind {
      case .revisionConflict:
        saveError = "別のウインドウまたは Agent が revision \(f.currentRev ?? 0) を保存しました。変更は保持しています。コピーしてから再読込してください。"
        remoteChangePending = true
      case .hashConflict:
        saveError = "\(f.args["path"] ?? "ソース") が編集開始後に変更されました。変更は保持しています。"
      case .externalChange:
        saveError = "\(f.args["path"] ?? "ファイル") が Tsugumi の外で変更されました。再読込で取り込めます（変更は保持しています）。"
      case .busy:
        saveError = "実行中のため Card の変更は保存できません。実行が終わってから保存してください（配置の変更は保存できます）。"
      default:
        saveError = f.description
      }
    } catch {
      // The reply may have been lost after the save committed: ask the core.
      if let r = try? await client.operation(op), r.state == .completed {
        saveError = nil
        if snapshot() == saved {
          // Committed: the DocPatched notification brings the confirmed copy.
          sourceEdits = [:]
          sourceRemovals = []
          definitionEdits = [:]
          draftBase = nil
        }
      } else {
        saveError = "保存できませんでした: \(error)。変更は保持しています。"
      }
    }
  }

  private func applyDeltaFromSave(_ d: Tsugumi_RPC_DocumentDeltaT) {
    if d.baseRev == rev {
      let keep = snapshot()
      applyDelta(d)
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

  func refreshVisibleDisplays() async {
    guard let client, let subscription else { return }
    // What a Card shows is up to its code (the display SDK): every Card that
    // can run may send displays.
    let canvasCards = resolution.keys.filter { [.code, .logger].contains(kind($0)) }
    try? await client.setVisible(subscription: subscription, cards: canvasCards.map(path), enabled: true)
  }

  // MARK: - Runs

  func run(card: [String], propagate: Bool) async throws {
    guard let client else { throw RPCError.disconnected }
    if computationDirty { throw RPCFailure(kind: .validationFailed, diagnostic: "実行する前に変更を保存してください。") }
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
      if prefix.isEmpty { link = links[id]?["external"]?.string } else { link = externals[prefix]?.links[id] }
      guard let name = link else { return nil }
      prefix += "externals/\(name)/"
    }
    return prefix
  }

  /// Whether a scope is inside a Card in Card (read-only here).
  func isExternalScope(_ scope: [String]) -> Bool { !scope.isEmpty }

  private func graph(_ scope: [String]) -> JSONValue? {
    guard let prefix = projectPrefix(scope) else { return nil }
    return prefix.isEmpty ? project["canvas"] : externals[prefix]?.project["canvas"]
  }

  /// Cards of a scope: this project's draft, or an external's (confirmed).
  func cards(in scope: [String]) -> [JSONValue] { graph(scope)?["cards"]?.array ?? [] }

  func connections(in scope: [String]) -> [JSONValue] { graph(scope)?["connections"]?.array ?? [] }

  /// Mutates this project's canvas in the draft (externals are read-only).
  func updateGraph(_ scope: [String], _ fn: (inout JSONValue) -> Void) {
    guard scope.isEmpty else { return }
    var canvas = project["canvas"] ?? .object([("cards", .array([])), ("connections", .array([]))])
    fn(&canvas)
    project["canvas"] = canvas
  }

  func instance(_ k: CardKey) -> JSONValue? {
    let p = path(k)
    guard let last = p.last else { return nil }
    return cards(in: Array(p.dropLast())).first { $0["id"]?.string == last }
  }

  /// The Card's settings (its _card.json), from the draft or an external.
  func cardDefinition(_ k: CardKey) -> JSONValue? {
    let p = path(k)
    guard let id = p.last, let prefix = projectPrefix(Array(p.dropLast())) else { return nil }
    return prefix.isEmpty ? definitions[id] : externals[prefix]?.definitions[id]
  }

  /// The external project of a Card in Card ("externals/<name>/…"), if it is one.
  func cardExternal(_ k: CardKey) -> String? {
    let p = path(k)
    guard let id = p.last, let prefix = projectPrefix(Array(p.dropLast())) else { return nil }
    let name = prefix.isEmpty ? links[id]?["external"]?.string : externals[prefix]?.links[id]
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
    return externals[ext]?.project["boundary"]
  }

  /// Changes this project's Card's _card.json in the draft.
  func updateCard(_ k: CardKey, _ fn: (inout JSONValue) -> Void) {
    let p = path(k)
    guard p.count == 1, let id = p.first, var card = definitions[id] else { return }
    fn(&card)
    setDefinition(id, card: card, meta: definitionMeta[id])
  }

  /// A setting of the Card (its _card.json).
  func cardSetting(_ k: CardKey, _ name: String) -> JSONValue? { cardDefinition(k)?[name] }

  func position(_ k: CardKey) -> CGPoint {
    var p = layout["cards"]?[k]
    // Inside a Card in Card: the external's own layout.
    let parts = path(k)
    if p == nil, parts.count > 1, let prefix = projectPrefix(Array(parts.dropLast())) {
      p = externals[prefix]?.layout["cards"]?[parts.last!]
    }
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
    let known = Set(sources.keys)
    Task { @MainActor in
      var found: [String: ExternalProject] = [:]
      for p in paths where p.hasSuffix("/tsugumi.json") {
        let prefix = String(p.dropLast("tsugumi.json".count))
        // Only folders that are externals of a project: …externals/<name>/tsugumi.json.
        let parts = prefix.split(separator: "/")
        guard parts.count >= 2, parts.count % 2 == 0, parts[parts.count - 2] == "externals" else { continue }
        func read(_ rel: String) async -> JSONValue? {
          guard known.contains(rel), let d = try? await client.readFile(path: rel, atRev: atRev) else { return nil }
          return try? JSONValue.parse(String(decoding: d, as: UTF8.self))
        }
        guard let proj = await read(p) else { continue }
        var ext = ExternalProject(project: proj, layout: await read(prefix + "layout.json") ?? .object([]), definitions: [:], links: [:])
        for c in proj["canvas"]?["cards"]?.array ?? [] {
          guard let id = c["id"]?.string else { continue }
          let dir = prefix + "definitions/\(id)/"
          if let l = await read(dir + "_link.json"), let name = l["external"]?.string { ext.links[id] = name }
          else if let card = await read(dir + "_card.json") { ext.definitions[id] = card }
        }
        found[prefix] = ext
      }
      if self.externalsKey == key { self.externals = found }
    }
  }

  func setPosition(_ k: CardKey, _ pt: CGPoint) {
    var cards = layout["cards"] ?? .object([])
    cards[k] = .object([("x", .real((pt.x * 10).rounded() / 10)), ("y", .real((pt.y * 10).rounded() / 10))])
    layout["cards"] = cards
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
      lock.lock()
      if !items.isEmpty || done {
        let batch = items
        items.removeAll(keepingCapacity: true)
        let d = done
        lock.unlock()
        return (batch, d)
      }
      lock.unlock()
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
