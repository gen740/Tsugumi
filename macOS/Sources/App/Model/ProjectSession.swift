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

  init(_ d: Tsugumi_Data_DiagnosticT) {
    error = d.severity != .warning
    code = d.code
    message = d.message
    card = d.card?.path
    path = d.path
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

struct CardRunState {
  var state: Tsugumi_Data_CardState
  var invocation: UInt64
  var phase: Tsugumi_Data_Phase
  var failure: String?
}

/// One invocation on the run timeline (times in ms since 1970, from the core).
struct TimelineEntry: Identifiable, Hashable {
  var id: String { "\(card)#\(invocation)" }
  let card: CardKey
  let invocation: UInt64
  var state: Tsugumi_Data_CardState
  var startedAt: Int64?
  var endedAt: Int64?
}

/// Draft of one Card definition (`_card.json` and `_meta.json`), or its removal.
struct DefinitionDraft: Equatable {
  var card: JSONValue
  var meta: JSONValue?
  var removed = false
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
  private(set) var sources: [String: (hash: String, size: UInt64)] = [:]
  private(set) var resolution: [CardKey: ResolvedCard] = [:]
  private(set) var diagnostics: [Diagnostic] = []

  // Draft (uncommitted, managed here)
  var project: JSONValue = .object([])
  var layout: JSONValue = .object([])
  var sourceEdits: [String: Data] = [:]  // path -> new bytes
  var sourceRemovals: Set<String> = []
  var definitionEdits: [String: DefinitionDraft] = [:]
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

  func stopEnvironment() {
    Task {
      await client?.close()
      connected = false
      try? await environments.stop(identity)
    }
  }

  private func listen(_ c: RPCClient) {
    listenTask = Task { [weak self] in
      for await event in c.notifications {
        guard let self else { return }
        await self.handle(event)
      }
      guard let self, !Task.isCancelled else { return }
      self.connected = false
      self.reconnect()
    }
  }

  private func handle(_ e: NotificationEvent) async {
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
    for d in s.definitions.compactMap({ $0 }) {
      confirmedDefinitions[d.id] = try? JSONValue.parse(d.cardJson)
      if let m = d.metaJson { confirmedDefinitionMeta[d.id] = try? JSONValue.parse(m) }
    }
    sources = [:]
    for src in s.sources.compactMap({ $0 }) { sources[src.path] = (src.hash ?? "", src.sizeBytes ?? 0) }
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
    for def in d.definitionsUpsert.compactMap({ $0 }) {
      confirmedDefinitions[def.id] = try? JSONValue.parse(def.cardJson)
      confirmedDefinitionMeta[def.id] = def.metaJson.flatMap { try? JSONValue.parse($0) }
    }
    for id in d.definitionsRemoved.compactMap({ $0 }) { confirmedDefinitions[id] = nil; confirmedDefinitionMeta[id] = nil }
    for s in d.sourcesUpsert.compactMap({ $0 }) { sources[s.path] = (s.hash ?? "", s.sizeBytes ?? 0) }
    for p in d.sourcesRemoved.compactMap({ $0 }) { sources[p] = nil }
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
      }
    case .runstatuschanged:
      if let s = (v.value as? Tsugumi_RPC_RunStatusChangedT)?.status, s.runId == runStatus?.runId || runStatus == nil {
        runStatus = s
      }
    case .runcardchanged:
      if let inv = (v.value as? Tsugumi_RPC_RunCardChangedT)?.invocation {
        let k = key(inv.card.path)
        let prev = cardStates[k]
        if prev == nil || prev!.invocation <= (inv.invocation ?? 0) {
          cardStates[k] = CardRunState(state: inv.state, invocation: inv.invocation ?? 0, phase: inv.phase,
                                       failure: inv.failure?.diagnostic)
        }
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
    default: return
    }
    displays[k] = DisplaySnapshot(runId: d.runId, invocation: d.invocation ?? 0, revision: rev, value: value)
  }

  private func recordTimeline(_ k: CardKey, _ inv: Tsugumi_RPC_InvocationSummaryT) {
    let n = inv.invocation ?? 0
    if let i = timeline.firstIndex(where: { $0.card == k && $0.invocation == n }) {
      timeline[i].state = inv.state
      timeline[i].startedAt = inv.startedAt ?? timeline[i].startedAt
      timeline[i].endedAt = inv.endedAt ?? timeline[i].endedAt
    } else {
      timeline.append(TimelineEntry(card: k, invocation: n, state: inv.state, startedAt: inv.startedAt, endedAt: inv.endedAt))
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
        changes.append(Change.putDefinition(id: id, cardJSON: d.card.serialized(), metaJSON: d.meta?.serialized()))
      }
    }
    for (p, bytes) in sourceEdits.sorted(by: { $0.key < $1.key }) where !sourceRemovals.contains(p) {
      changes.append(Change.putSource(path: p, bytes: bytes, baseHash: base.hashes[p]))
    }
    for p in sourceRemovals.sorted() {
      if let h = base.hashes[p] { changes.append(Change.removeSource(path: p, baseHash: h)) }
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
    let canvasCards = resolution.keys.filter { cardSetting($0, "canvasEnabled")?.bool == true }
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
        try Data(d.cardJson.utf8).write(to: dir.appendingPathComponent("_card.json"))
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

  /// Graph cards of a scope in the draft (inline subgraphs only).
  func cards(in scope: [String]) -> [JSONValue] {
    var graph = project["canvas"]
    for id in scope {
      guard let card = graph?["cards"]?.array?.first(where: { $0["id"]?.string == id }) else { return [] }
      graph = card["canvas"]
    }
    return graph?["cards"]?.array ?? []
  }

  func connections(in scope: [String]) -> [JSONValue] {
    var graph = project["canvas"]
    for id in scope {
      guard let card = graph?["cards"]?.array?.first(where: { $0["id"]?.string == id }) else { return [] }
      graph = card["canvas"]
    }
    return graph?["connections"]?.array ?? []
  }

  /// Mutates the graph object of `scope` in the draft.
  func updateGraph(_ scope: [String], _ fn: (inout JSONValue) -> Void) {
    func recurse(_ graph: inout JSONValue, _ rest: ArraySlice<String>) {
      guard let id = rest.first else { fn(&graph); return }
      guard var cards = graph["cards"]?.array, let i = cards.firstIndex(where: { $0["id"]?.string == id }),
            var inner = cards[i]["canvas"] else { return }
      recurse(&inner, rest.dropFirst())
      cards[i]["canvas"] = inner
      graph["cards"] = .array(cards)
    }
    var canvas = project["canvas"] ?? .object([("cards", .array([])), ("connections", .array([]))])
    recurse(&canvas, scope[...])
    project["canvas"] = canvas
  }

  func instance(_ k: CardKey) -> JSONValue? {
    let p = path(k)
    guard let last = p.last else { return nil }
    return cards(in: Array(p.dropLast())).first { $0["id"]?.string == last }
  }

  func updateInstance(_ k: CardKey, _ fn: (inout JSONValue) -> Void) {
    let p = path(k)
    guard let last = p.last else { return }
    updateGraph(Array(p.dropLast())) { g in
      guard var cards = g["cards"]?.array, let i = cards.firstIndex(where: { $0["id"]?.string == last }) else { return }
      fn(&cards[i])
      g["cards"] = .array(cards)
    }
  }

  /// Effective setting: instance override, else definition default.
  func cardSetting(_ k: CardKey, _ name: String) -> JSONValue? {
    if let v = instance(k)?[name] { return v }
    if let def = instance(k)?["definition"]?.string { return definitions[def]?[name] }
    return nil
  }

  func position(_ k: CardKey) -> CGPoint {
    let p = layout["cards"]?[k]
    return CGPoint(x: p?["x"]?.double ?? 0, y: p?["y"]?.double ?? 0)
  }

  func setPosition(_ k: CardKey, _ pt: CGPoint) {
    var cards = layout["cards"] ?? .object([])
    cards[k] = .object([("x", .real((pt.x * 10).rounded() / 10)), ("y", .real((pt.y * 10).rounded() / 10))])
    layout["cards"] = cards
  }
}
