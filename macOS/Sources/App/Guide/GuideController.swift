import FlatBuffers
import Foundation
import Observation
import SwiftUI
import TsugumiAgent
import TsugumiRPC

/// A button the guide offers under its reply (guide_show); the user presses
/// it and the app does it with its own controls.
struct GuideAction: Identifiable, Hashable {
  enum Kind: String {
    case openDemo = "open_demo", showDemos = "show_demos", run, stop, openCard = "open_card", openSettings = "open_settings"
    case newProject = "new_project", home
  }

  let id = UUID()
  let kind: Kind
  let label: String
  var demo: String?
  var card: String?

  /// What a guide_show call points at and offers.
  static func parse(_ arguments: String) -> (highlight: String?, actions: [GuideAction]) {
    guard let v = try? JSONValue.parse(arguments) else { return (nil, []) }
    let actions = (v["actions"]?.array ?? []).compactMap { a -> GuideAction? in
      guard let kind = a["kind"]?.string.flatMap(Kind.init(rawValue:)) else { return nil }
      let label = a["label"]?.string?.trimmingCharacters(in: .whitespaces) ?? ""
      return GuideAction(kind: kind, label: label.isEmpty ? kind.rawValue : label, demo: a["demo"]?.string, card: a["card"]?.string)
    }
    let highlight = v["highlight"]?.string?.trimmingCharacters(in: .whitespaces)
    // Unknown kinds are dropped first; at most three buttons.
    return (highlight?.isEmpty == false ? highlight : nil, Array(actions.prefix(3)))
  }
}

/// The guide reads the project on the user's screen; on Home there is none.
private struct GuideNoProject: Error, CustomStringConvertible {
  var description: String { Agent.guideText("noProject") }
}

/// The guide's reads go to the project of the window the user asked from.
private struct GuideTransport: ToolTransport {
  let replyNames: [UInt8: String]

  func call(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset) async throws -> (replyType: String, table: FlatBuffers.Table) {
    guard let session = await GuideController.shared.readSession, let rpc = await session.rpc else { throw GuideNoProject() }
    let (type, table) = try await rpc.callRaw(payloadType: payloadType, build: build)
    return (replyNames[type] ?? "Unknown", table)
  }
}

/// The guide bird in the bottom-right corner of every main window: one
/// conversation for the whole app with a model of the user's own (the same
/// connection and key as the Agent; without a key there is no guide at all).
/// It explains what is on the screen and leads demo tours; it only reads,
/// and what it offers to do is done by the user's own press of a button.
@MainActor
@Observable
final class GuideController {
  static let shared = GuideController()

  struct Item: Identifiable {
    let id = UUID()
    /// "user", "assistant" or "note" (what the app told the guide: a tour step, a run that ended).
    var role: String
    var text: String
    var actions: [GuideAction] = []
  }

  /// The demo being shown: its id and title and the copy's folder. Kept
  /// across launches; `started` once the guide began it.
  struct Tour: Codable, Equatable {
    var demo: String
    var title: String
    var path: String
    var started = false
  }

  /// What the window the user asks from shows.
  enum Screen: Equatable { case home, demos, project }

  /// A model connection the guide can use (a key for the chosen provider).
  private(set) var available = false
  private(set) var items: [Item] = []
  private(set) var busy = false
  private(set) var failed = false
  private(set) var status: String?
  /// What the guide is doing now, in a few words (the bubble under the bird).
  private(set) var activity: String?
  /// The panel is open (in every main window; each shows its own).
  var open = false
  /// The part of the screen the guide points at (guide_show), for a while.
  private(set) var highlight: String?
  private(set) var tour: Tour?
  /// Home's demo list asked for by the guide (show_demos).
  var demosRequested = false

  /// The first start's question (provider and key, or skip) was answered.
  private(set) var setupDone: Bool
  /// "デモを見ますか？" was answered (見る or あとで).
  private(set) var demoAnswered: Bool

  /// The first start asks for a key: nothing usable is set and it was never answered.
  var needsSetup: Bool { !setupDone && !available && !Self.quiet }
  /// The guide greets and offers the demo once, when it first has a connection.
  var offersDemo: Bool { available && !demoAnswered && !Self.quiet }

  /// Scripted checks and smoke runs: no first-start questions over the windows.
  static let quiet = ProcessInfo.processInfo.environment["TSUGUMI_UI_SCRIPT"] != nil
    || ProcessInfo.processInfo.environment["TSUGUMI_SMOKE_DEMO"] != nil

  @ObservationIgnored private var agent: Agent?
  @ObservationIgnored private var replyNames: [UInt8: String] = [:]
  @ObservationIgnored private var catalog: ToolCatalog?
  @ObservationIgnored private(set) weak var readSession: ProjectSession?
  @ObservationIgnored private weak var tourTab: WorkflowTab?
  /// Notes for the guide that came while it answered: sent when it is done.
  @ObservationIgnored private var queued: [(prompt: String, display: String)] = []
  /// Actions of the reply being written (shown when it ends).
  @ObservationIgnored private var pending: [GuideAction] = []
  /// The last run state seen in the tour's project, and whether a run was active then.
  @ObservationIgnored private var seen: String?
  @ObservationIgnored private var seenActive = false
  @ObservationIgnored private var highlightTask: Task<Void, Never>?
  @ObservationIgnored private let storage: URL

  private init() {
    let d = UserDefaults.standard
    setupDone = d.bool(forKey: "guide.setupDone")
    demoAnswered = d.bool(forKey: "guide.demoAnswered")
    tour = d.data(forKey: "guide.tour").flatMap { try? JSONDecoder().decode(Tour.self, from: $0) }
    storage = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Tsugumi/agent/guide/conversation.json")
    if let bfbs = RPCSchema.bfbs(), let schema = try? BFBS(data: bfbs), let catalog = try? ToolCatalog(schema: schema) {
      self.catalog = catalog
      if let e = schema.enums.first(where: { $0.name == "Tsugumi.RPC.ReplyPayload" }) {
        for v in e.values { replyNames[UInt8(v.value)] = v.name }
      }
    }
    refreshAvailability()
    load(Conversation.load(storage))
  }

  // MARK: - Connection

  /// Reads whether the chosen provider has a key (after Settings or the first
  /// start changed it). A key already there answers the first start's question.
  func refreshAvailability() {
    let config = AgentSettings.current()
    available = catalog != nil && (!config.usesKeychain || Keychain.load(account: config.name) != nil)
    if available && !setupDone { setupDone = true; UserDefaults.standard.set(true, forKey: "guide.setupDone") }
    if !available {
      open = false
      if busy { cancel() }
      clearHighlight()
    }
  }

  /// The first start was answered: with a key that connected, or skipped.
  func finishSetup() {
    setupDone = true
    UserDefaults.standard.set(true, forKey: "guide.setupDone")
    refreshAvailability()
    if offersDemo { open = true }
  }

  /// Asks the first start's questions again (Settings).
  func resetIntroduction() {
    setupDone = false
    demoAnswered = false
    UserDefaults.standard.set(false, forKey: "guide.setupDone")
    UserDefaults.standard.set(false, forKey: "guide.demoAnswered")
    refreshAvailability()
    if offersDemo { open = true }
  }

  /// "デモを見ますか？": either answer is kept, so it is never asked again.
  func answerDemoOffer() {
    demoAnswered = true
    UserDefaults.standard.set(true, forKey: "guide.demoAnswered")
  }

  func toggleFromMenu() {
    guard available else { return }
    open.toggle()
  }

  // MARK: - Conversation

  private func makeAgent() -> Agent? {
    if let agent { return agent }
    guard let catalog else { return nil }
    let a = Agent(catalog: catalog, transport: GuideTransport(replyNames: replyNames), provider: AgentSettings.current(),
                  storage: storage, references: ChatController.references, role: .guide)
    Task {
      // A guide answers in a few steps and never waits on a run.
      await a.setMaxSteps(12)
      await a.setStatusWait(0)
    }
    agent = a
    return a
  }

  /// A question of the user, with what their window shows.
  func ask(_ text: String, screen: Screen, session: ProjectSession?, tab: WorkflowTab?) {
    let text = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard available, !busy, !text.isEmpty, let agent = makeAgent() else { return }
    readSession = session
    clearHighlight()
    items.append(Item(role: "user", text: text))
    let prompt = text + "\n\n" + screenNote(screen, session: session, tab: tab)
    begin()
    Task {
      await agent.setProvider(AgentSettings.current())
      await agent.send(prompt: prompt, title: text, display: text) { e in Task { @MainActor in self.handle(e) } }
    }
  }

  /// Something the app tells the guide (a tour step, a run that ended); one
  /// that comes while it answers waits for the answer to end.
  private func tell(_ prompt: String, display: String, session: ProjectSession?) {
    guard available, let agent = makeAgent() else { return }
    if busy {
      queued.append((prompt, display))
      return
    }
    if let session { readSession = session }
    items.append(Item(role: "note", text: display))
    let full = prompt + "\n\n" + screenNote(.project, session: readSession, tab: tourTab)
    begin()
    Task {
      await agent.setProvider(AgentSettings.current())
      await agent.send(prompt: full, title: display, display: display) { e in Task { @MainActor in self.handle(e) } }
    }
  }

  func cancel() {
    queued = []
    Task { await agent?.cancel() }
    busy = false
    activity = nil
    status = String(localized: "回答を止めました。")
  }

  func retry() {
    guard available, !busy, let agent = makeAgent() else { return }
    begin()
    Task {
      await agent.setProvider(AgentSettings.current())
      await agent.retry { e in Task { @MainActor in self.handle(e) } }
    }
  }

  /// A new conversation (the old one is gone; the tour stays).
  func newConversation() {
    guard !busy else { return }
    Task { await agent?.reset() }
    items = []
    status = nil
    failed = false
    clearHighlight()
  }

  private func begin() {
    busy = true
    failed = false
    status = nil
    pending = []
    activity = String(localized: "考えています")
  }

  private func handle(_ e: AgentEvent) {
    switch e {
    case .text(let t):
      activity = String(localized: "考えています")
      if let last = items.last, last.role == "assistant" {
        items[items.count - 1].text += t
      } else {
        items.append(Item(role: "assistant", text: t))
      }
    case .toolStarted(_, let name, let args):
      if name == "guide_show" {
        let shown = GuideAction.parse(args)
        if let h = shown.highlight { point(at: h) }
        pending += shown.actions
      } else {
        activity = name.hasPrefix("run") || name == "log_range" ? String(localized: "実行の様子を見ています")
          : String(localized: "プロジェクトを読んでいます")
      }
    case .toolFinished:
      break
    case .discardText:
      if let last = items.last, last.role == "assistant" { items.removeLast() }
    case .retrying(let s):
      status = String(localized: "再試行中: \(s)")
    case .compacted, .helper:
      break
    case .finished:
      ended()
      let reply = items.last { $0.role == "assistant" }?.text ?? ""
      if !reply.isEmpty { AccessibilityNotification.Announcement(String(reply.prefix(300))).post() }
      flush()
    case .stepLimit:
      ended()
      status = String(localized: "答えをまとめきれませんでした。聞き方を変えてもう一度どうぞ。")
    case .error(let s):
      ended()
      queued = []
      failed = s != "cancelled"
      status = s == "cancelled" ? String(localized: "回答を止めました。") : String(localized: "うまく答えられませんでした: \(s)")
    }
  }

  private func ended() {
    busy = false
    activity = nil
    guard !pending.isEmpty else { return }
    // Only the newest buttons are offered.
    for i in items.indices { items[i].actions = [] }
    if let i = items.lastIndex(where: { $0.role == "assistant" }), i == items.count - 1 {
      items[i].actions = pending
    } else {
      items.append(Item(role: "assistant", text: "", actions: pending))
    }
    pending = []
  }

  private func flush() {
    guard !queued.isEmpty else { return }
    let notes = queued
    queued = []
    tell(notes.map(\.prompt).joined(separator: "\n"), display: notes.last!.display, session: readSession)
  }

  private func load(_ conv: Conversation) {
    var out: [Item] = []
    // The buttons of a guide_show go under the reply that follows it; only the newest stay.
    var shown: [GuideAction] = []
    for m in conv.fullHistory {
      if m.role == "user" {
        let isNote = m.content.contains { if case .text(let t) = $0 { return t.hasPrefix("[Tour]") || t.hasPrefix("[Event]") }; return false }
        if let d = m.display {
          if !shown.isEmpty { out.append(Item(role: "assistant", text: "", actions: shown)); shown = [] }
          out.append(Item(role: isNote ? "note" : "user", text: d))
        }
        continue
      }
      for b in m.content {
        switch b {
        case .text(let t):
          out.append(Item(role: "assistant", text: t, actions: shown))
          shown = []
        case .toolUse(_, let n, let a) where n == "guide_show":
          shown = GuideAction.parse(a).actions
        default: break
        }
      }
    }
    if !shown.isEmpty { out.append(Item(role: "assistant", text: "", actions: shown)) }
    if let newest = out.lastIndex(where: { !$0.actions.isEmpty }) {
      for i in out.indices where i != newest { out[i].actions = [] }
    }
    items = out
  }

  // MARK: - Pointing

  private func point(at target: String) {
    highlight = target
    highlightTask?.cancel()
    highlightTask = Task { @MainActor [weak self] in
      try? await Task.sleep(for: .seconds(12))
      guard !Task.isCancelled else { return }
      self?.highlight = nil
    }
  }

  func clearHighlight() {
    highlightTask?.cancel()
    highlight = nil
  }

  /// Whether the guide points at `target` (the run button is also "stop").
  func points(at target: String) -> Bool {
    guard let h = highlight else { return false }
    return h == target || (target == "run" && h == "stop")
  }

  // MARK: - What the guide sees

  /// [Screen]: what the user's window shows, for the guide to answer about.
  func screenNote(_ screen: Screen, session: ProjectSession?, tab: WorkflowTab?) -> String {
    var lines = ["[Screen]"]
    switch (screen, session) {
    case (.project, let s?):
      lines.append("screen: the project \"\(s.scopeName([]))\" (folder \(s.identity.url.path))")
      if let tab {
        lines.append("workflow shown: " + (tab.scope.isEmpty ? "the root workflow" : "the Card in Card \(tab.scope.joined(separator: "/"))"))
        let cards = s.cards(in: tab.scope).compactMap { $0["id"]?.string }
        lines.append("Cards in it: " + (cards.isEmpty ? "none" : cards.map { "\($0) (\(s.kind(key(tab.scope + [$0])).rawValue))" }.joined(separator: ", ")))
        let selected = tab.selection.sorted()
        lines.append("selected Cards: " + (selected.isEmpty ? "none" : selected.map { describe($0, in: s) }.joined(separator: "; ")))
      }
      if s.connected {
        let state = s.runStatus.map { String(describing: $0.state) } ?? "none yet"
        lines.append("run: \(state)" + (s.currentRunId.map { " (run \($0))" } ?? ""))
      } else {
        lines.append("connection: not connected to the project's environment (\(s.environment.label))")
      }
      if let t = tour, Self.same(t.path, s.identity.url) { lines.append("tour: this project is the copy of the demo \"\(t.demo)\" being shown") }
    case (.demos, _):
      lines.append("screen: Home, the demo list")
    default:
      lines.append("screen: Home (recent projects, new project, open, デモを見る)")
    }
    if let t = tour, session.map({ !Self.same(t.path, $0.identity.url) }) ?? true {
      lines.append("tour: the demo \"\(t.demo)\" is paused (the user is not in its copy)")
    }
    lines.append("demos: " + AppModel.shared.demos.map { "\($0.id) \"\($0.title)\"" }.joined(separator: ", "))
    return lines.joined(separator: "\n")
  }

  private func describe(_ k: CardKey, in s: ProjectSession) -> String {
    var d = "\(k) (\(s.kind(k).label), \"\(s.nodeData(k).title)\""
    if let st = s.cardStates[k] { d += ", last state \(st.state.label)" }
    if let r = s.resultSummary(k) { d += ", result: \(r.prefix(300))" }
    return d + ")"
  }

  nonisolated static func same(_ path: String, _ url: URL) -> Bool {
    URL(fileURLWithPath: path).resolvingSymlinksInPath().standardizedFileURL.path == url.resolvingSymlinksInPath().standardizedFileURL.path
  }

  // MARK: - Demo tours

  /// The user chose a demo: its copy is about to open; the tour starts there.
  func startTour(_ demo: DemoEntry, copy: URL) {
    answerDemoOffer()
    tour = Tour(demo: demo.id, title: demo.title, path: copy.path)
    seen = nil
    saveTour()
    open = true
  }

  func endTour() {
    tour = nil
    seen = nil
    saveTour()
  }

  /// The user asks to go on with a paused tour (its copy opens again).
  func resumeTour() -> URL? {
    guard let t = tour, FileManager.default.fileExists(atPath: URL(fileURLWithPath: t.path).appendingPathComponent("tsugumi.json").path) else {
      endTour()
      return nil
    }
    resumeAsked = true
    return URL(fileURLWithPath: t.path)
  }

  @ObservationIgnored private var resumeAsked = false

  private func saveTour() {
    UserDefaults.standard.set(tour.flatMap { try? JSONEncoder().encode($0) }, forKey: "guide.tour")
  }

  /// A window shows a project: in the tour's copy the guide begins the tour
  /// once it is connected, and follows its runs from their real states.
  func observe(_ s: ProjectSession, tab: WorkflowTab?) {
    guard available, let t = tour, Self.same(t.path, s.identity.url) else { return }
    tourTab = tab
    if !t.started {
      guard s.connected else { return }
      tour?.started = true
      saveTour()
      seen = runKey(s)
      seenActive = s.runActive
      tell(Agent.guideText("tourStart", ["demo": t.demo, "title": t.title]),
           display: String(localized: "デモ「\(t.title)」の案内を始めます"), session: s)
      return
    }
    if resumeAsked, s.connected {
      resumeAsked = false
      seen = runKey(s)
      seenActive = s.runActive
      tell(Agent.guideText("tourResume", ["demo": t.demo]), display: String(localized: "デモの案内を続けます"), session: s)
      return
    }
    let now = runKey(s)
    defer { seen = now; if s.connected { seenActive = s.runActive } }
    guard let before = seen, before != now else {
      if seen == nil { seen = now; seenActive = s.runActive }
      return
    }
    if !s.connected {
      // Lost while a run was going: its outcome is not known yet.
      if seenActive { tell(Agent.guideText("event", ["event": Agent.guideText("disconnected")]),
                           display: String(localized: "実行環境との接続が切れました"), session: s) }
      return
    }
    let state = s.runStatus.map { String(describing: $0.state) } ?? "none"
    let run = s.currentRunId ?? "-"
    if before == "offline" {
      if seenActive { tell(Agent.guideText("event", ["event": Agent.guideText("reconnected", ["state": state])]),
                           display: String(localized: "実行環境に再接続しました"), session: s) }
      return
    }
    if s.runActive {
      // A new run (not each step of one).
      if !before.hasPrefix(run + ":") || !seenActive {
        tell(Agent.guideText("event", ["event": Agent.guideText("runStarted", ["run": run])]),
             display: String(localized: "実行が始まりました"), session: s)
      }
    } else if seenActive || !before.hasPrefix(run + ":"), s.runStatus != nil {
      tell(Agent.guideText("event", ["event": Agent.guideText("runEnded", ["run": run, "state": state])]),
           display: String(localized: "実行が終わりました（\(s.runStatus?.state.label ?? state)）"), session: s)
    }
  }

  /// The run as the guide follows it: "offline", or "<run id>:<state>".
  private func runKey(_ s: ProjectSession) -> String {
    guard s.connected else { return "offline" }
    return "\(s.currentRunId ?? "-"):\(s.runStatus.map { String(describing: $0.state) } ?? "none")"
  }
}
