import FlatBuffers
import Foundation
import Observation
import SwiftUI
import TsugumiAgentClient
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

  /// What a guide_show call points at, says and offers.
  static func parse(_ arguments: String) -> GuideShow {
    guard let v = try? JSONValue.parse(arguments) else { return GuideShow() }
    let actions = (v["actions"]?.array ?? []).compactMap { a -> GuideAction? in
      guard let kind = a["kind"]?.string.flatMap(Kind.init(rawValue:)) else { return nil }
      let label = a["label"]?.string?.trimmingCharacters(in: .whitespaces) ?? ""
      return GuideAction(kind: kind, label: label.isEmpty ? kind.rawValue : label, demo: a["demo"]?.string, card: a["card"]?.string)
    }
    let highlight = v["highlight"]?.string?.trimmingCharacters(in: .whitespaces)
    let say = v["say"]?.string?.trimmingCharacters(in: .whitespacesAndNewlines)
    let choices = (v["choices"]?.array ?? []).compactMap { $0.string?.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
    // Unknown kinds are dropped first; at most three buttons and four choices.
    return GuideShow(highlight: highlight?.isEmpty == false ? highlight : nil, say: say?.isEmpty == false ? say : nil,
                     choices: Array(choices.prefix(4)), actions: Array(actions.prefix(3)), endTour: v["endTour"]?.bool == true,
                     open: v["open"]?.string?.trimmingCharacters(in: .whitespaces))
  }
}

/// A guide_show call: where the bird flies, what it says there, the replies
/// it offers to tap and the buttons that act.
struct GuideShow: Equatable {
  var highlight: String?
  var say: String?
  var choices: [String] = []
  var actions: [GuideAction] = []
  /// The last step of a demo tour: no more notes about runs.
  var endTour = false
  /// A window to open for the user to look at ("card:<path>": that Card's settings).
  var open: String?
}

/// What the bird says in its speech bubble, where it is (target: what it
/// points at; nil: in its corner), and what can be tapped there.
struct GuideBubble: Equatable {
  let id = UUID()
  var say: String?
  var choices: [String] = []
  var actions: [GuideAction] = []
  var target: String?
  /// The guide is answering (the bubble shows that it thinks).
  var thinking = false
  /// The tour's last words: the bird says them, then flies home by itself.
  var farewell = false
  /// The answer says more than the bubble: it points the user to the chat (or the panel).
  var more = false
  /// Said by the app (a demo picked from the first questions), not by a model:
  /// its Other demos asks those questions again.
  var local = false
}

/// The guide reads the project on the user's screen; on Home there is none.
private struct GuideNoProject: Error, CustomStringConvertible {
  var description: String { Agent.guideText("noProject") }
}

/// The guide's reads go to the project of the window the user asked from.
private struct GuideTransport: ToolTransport {
  @MainActor func project() async throws -> AgentProject? {
    guard let session = GuideController.shared.readSession else { return nil }
    return try await RPCToolTransport(session: session).project()
  }
}

/// The guide bird in the bottom-right corner of every main window: one
/// conversation for the whole app with a model of the user's own (the same
/// connection and key as the Agent). Without a connection it answers from a
/// fixed script in agentd: one demo's tour, and a pointer to Settings.
/// It explains what is on the screen and leads demo tours; it only reads,
/// and what it offers to do is done by the user's own press of a button.
/// What it points at and says stays in the windows it is about: Home for the
/// guide's own, a project's for the chat of that project (`speaks(in:)`).
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
    /// Replies to tap (guide_show "choices"), under the newest reply only.
    var choices: [String] = []
  }

  /// The demo being shown: its id and title and the copy's folder. Kept
  /// across launches; `started` once the guide began it.
  struct Tour: Equatable {
    var demo: String
    var title: String
    var path: String
    var started = false
  }

  /// What the window the user asks from shows.
  enum Screen: Equatable { case home, demos, project }

  /// The bird is there: with a model connection, or answering from its script.
  private(set) var available = false
  /// A model connection the guide can use (a key for the chosen provider).
  private(set) var connected = false
  /// No connection: Tsugumi answers from agentd and calls no model.
  var scripted: Bool { available && !connected }
  private(set) var items: [Item] = []
  private(set) var busy = false
  private(set) var failed = false
  private(set) var status: String?
  /// What the guide is doing now, in a few words (the bubble under the bird).
  private(set) var activity: String?
  /// The panel is open (in every main window; each shows its own).
  var open = false
  /// The part of the screen the guide points at and the bird flies to (guide_show).
  private(set) var highlight: String?
  /// The bird's speech bubble (nil: it sits quietly in its corner).
  private(set) var bubble: GuideBubble?
  /// What to open for the user to look at, taken by the main window that shows
  /// the project: "card:<path>" (that Card's settings), a panel under the canvas
  /// ("logs", "timeline", "terminal") or "versions".
  var openRequest: String?

  /// The panel under the canvas an `open` names.
  nonisolated static func panel(_ open: String) -> BottomPanel? {
    switch open {
    case "logs": return .log
    case "timeline": return .timeline
    case "terminal": return .terminal
    default: return nil
    }
  }
  /// A workflow scope to show (pointing into a Card in Card), taken by the project's window.
  var scopeRequest: [String]?
  /// Windows where the bird is out, by the project they show ("": Home). Its
  /// homes for that project show no bird meanwhile: one Tsugumi per project.
  private var awayWindows: [String: Int] = [:]
  func birdAway(_ project: String?) -> Bool { awayWindows[project ?? "", default: 0] > 0 }
  func birdLeft(_ project: String?, by change: Int) { awayWindows[project ?? "", default: 0] += change }
  /// The chat's answer the bubble points to was asked for: the chat shows it marked.
  private(set) var chatFlash: Date?

  /// The bubble's "More in the chat": the chat (or on Home the panel) shows the whole answer.
  func showDetails() {
    if speakingChat != nil { chatFlash = Date() } else { open = true }
  }

  /// Whether an answer says more than its bubble.
  nonisolated static func saysMore(_ reply: String, than say: String?) -> Bool {
    reply.trimmingCharacters(in: .whitespacesAndNewlines).count > (say?.count ?? 0) + 120
  }
  private(set) var tour: Tour?
  /// Home's demo list asked for by the guide (show_demos).
  var demosRequested = false

  /// The first start's question (provider and key, or skip) was answered.
  private(set) var setupDone = true
  /// The offer of a demo was answered (Show Me or Later).
  private(set) var demoAnswered = true

  /// The first start asks for a key: nothing usable is set and it was never answered.
  var needsSetup: Bool { !setupDone && !connected && !Self.quiet }
  /// The guide greets and offers the demo once (without a connection, the scripted tour).
  var offersDemo: Bool { available && !demoAnswered && !Self.quiet }
  /// The main window (its Workspace id) that asks "Would you like to see the guide?": the
  /// user's own save of a key (or sign-in) has just made the guide usable.
  var demoInvite: UUID?
  @ObservationIgnored private var inviteOnConnect = false

  /// The first questions on Home, written by the app (no model): what to try,
  /// then a field or the Drafts, then a demo.
  enum OfferStep: Equatable { case start, fields, field(String), drafts }
  var offerStep: OfferStep = .start
  /// Other demos by a demo the app pointed at: the questions are out again.
  private(set) var asksAgain = false
  /// The corner shows the first questions.
  var asksDemo: Bool { available && !Self.quiet && (!demoAnswered || asksAgain) }
  static var otherDemos: String { String(localized: "ほかのデモ") }

  /// Scripted checks and smoke runs: no first-start questions over the windows.
  static let quiet = ProcessInfo.processInfo.environment["TSUGUMI_UI_SCRIPT"] != nil
    || ProcessInfo.processInfo.environment["TSUGUMI_SMOKE_DEMO"] != nil

  @ObservationIgnored private var agent: Agent?
  @ObservationIgnored private(set) weak var readSession: ProjectSession?
  @ObservationIgnored private weak var tourTab: WorkflowTab?
  /// Notes for the guide that came while it answered: sent when it is done.
  @ObservationIgnored private var queued: [(prompt: String, display: String)] = []
  /// Actions and choices of the reply being written (shown when it ends).
  @ObservationIgnored private var pending: [GuideAction] = []
  @ObservationIgnored private var pendingChoices: [String] = []
  /// What the window the user last looked from shows (choices are asked with it).
  @ObservationIgnored private var contextScreen: Screen = .home
  @ObservationIgnored private weak var contextSession: ProjectSession?
  @ObservationIgnored private weak var contextTab: WorkflowTab?
  /// The last screen snapshot reported, to avoid redundant observations.
  @ObservationIgnored private var observed: String?
  @ObservationIgnored private var highlightTask: Task<Void, Never>?

  private init() {
    refreshAvailability()
  }

  // MARK: - Connection

  /// Reads whether the chosen provider can be used: a key (Anthropic, OpenAI),
  /// a signed-in ChatGPT plan, or the local fixture (after Settings or the
  /// first start changed it). One already there answers the first start's question.
  func refreshAvailability() {
    let config = AgentSettings.current()
    switch config.auth {
    case .none: apply(true)
    case .apiKey: Task { apply(ProviderCredentials.exists(account: config.name)) }
    case .chatGPT:
      Task {
        let accounts = await ChatGPTAuth.shared().accounts()
        apply(accounts.contains { $0.active && $0.signedIn && $0.planEnabled })
      }
    }
  }

  private func apply(_ usable: Bool) {
    available = true
    connected = available && usable
    advance(["kind": "connection", "connected": connected])
    // A key the user just saved connected the guide: the main window in front
    // asks whether to see the demo (with the model now, not the script).
    if inviteOnConnect {
      inviteOnConnect = false
      if connected && !Self.quiet { demoInvite = AppModel.shared.frontMainWindow?.id }
    }
    if !connected && busy { cancel() }
  }

  /// The user saved a key or signed in (Settings): when that is what makes the
  /// guide usable, the main window in front asks whether to see the demo.
  func connectionSaved() {
    inviteOnConnect = !connected
    refreshAvailability()
  }

  /// The first start was answered: with a key that connected (the main window
  /// then asks whether to see the demo), or skipped.
  func finishSetup(connected: Bool = false) {
    if connected { inviteOnConnect = true }
    setupDone = true
    advance(["kind": "setup"])
    refreshAvailability()
  }

  /// Asks the first start's questions again (Settings).
  func resetIntroduction() {
    setupDone = false
    demoAnswered = false
    offerStep = .start
    advance(["kind": "introduction"])
    refreshAvailability()
  }

  /// The screen a window shows (its corner reports it), for choices tapped in a bubble.
  func setContext(_ screen: Screen, session: ProjectSession?, tab: WorkflowTab?) {
    contextScreen = screen
    contextSession = session
    contextTab = tab
    if let session { shownTabs[session.identity.id] = ShownTab(tab: tab) }
  }

  /// The workflow tab each open project last showed (the bubble's buttons act on it).
  private struct ShownTab { weak var tab: WorkflowTab? }
  @ObservationIgnored private var shownTabs: [String: ShownTab] = [:]

  /// A choice tapped in the bubble or under a reply: the user's answer, to
  /// whichever Tsugumi offered it (a project's chat, or the guide on Home).
  func choose(_ choice: String) {
    if bubble?.local == true, choice == Self.otherDemos {
      dismissBubble()
      asksAgain = true
      return
    }
    if let chat = speakingChat {
      chat.send(choice)
    } else {
      ask(choice, screen: contextScreen, session: contextSession, tab: contextTab)
    }
  }

  // MARK: - Tsugumi of a project's chat

  /// The project chat whose bubble is out (nil: the guide's own on Home).
  @ObservationIgnored private(set) weak var speakingChat: ChatController?
  /// The project and workflow the flying bird's buttons act on.
  var flyerSession: ProjectSession? { speakingChat?.session ?? contextSession }
  var flyerTab: WorkflowTab? { flyerSession.flatMap { shownTabs[$0.identity.id]?.tab } ?? contextTab }

  /// Whether what the bird says now belongs in a window showing this project
  /// (nil: Home; another project's window goes on undisturbed).
  func speaks(in project: String?) -> Bool { speakingChat?.session.identity.url.path == project }

  /// The project of this chat closed: what it was saying goes with it.
  func chatClosed(_ chat: ChatController) {
    shownTabs[chat.session.identity.id] = nil
    guard speakingChat === chat else { return }
    speakingChat = nil
    dismissBubble()
  }

  func detach(from session: ProjectSession) async {
    guard readSession === session else { return }
    await agent?.detach()
    readSession = nil
    busy = false
  }

  /// The chat's Tsugumi called guide_show: the bird flies and speaks for it.
  func chatShow(_ chat: ChatController, arguments: String) {
    guard available else { return }
    speakingChat = chat
    show(GuideAction.parse(arguments))
  }

  /// A guide_show of either Tsugumi: where the bird goes, what opens, what it says.
  private func show(_ shown: GuideShow) {
    if shown.endTour && !scripted { endTour() }
    if let o = shown.open, o.hasPrefix("card:") || o == "versions" || Self.panel(o) != nil { openRequest = o }
    point(at: shown.highlight)
    bubble = GuideBubble(say: shown.say, choices: shown.choices, actions: shown.actions, target: shown.highlight,
                         farewell: shown.endTour)
  }

  /// After its last words the bird flies home by itself.
  private func leaveAfterFarewell() {
    guard let b = bubble, b.farewell, !b.thinking else { return }
    let id = b.id
    Task { @MainActor [weak self] in
      try? await Task.sleep(for: .seconds(7))
      if self?.bubble?.id == id { self?.dismissBubble() }
    }
  }

  /// The chat answers: a bubble that is out shows that it thinks.
  func chatTurnBegan(_ chat: ChatController) {
    guard speakingChat === chat, let b = bubble else { return }
    bubble = GuideBubble(target: b.target, thinking: true)
  }

  /// The chat's answer ended: a bubble still thinking says its first sentence.
  func chatTurnEnded(_ chat: ChatController, reply: String) {
    guard speakingChat === chat, var b = bubble else { return }
    if b.thinking { b = GuideBubble(target: b.target) }
    if b.say == nil { b.say = Self.firstSentence(reply) }
    b.more = Self.saysMore(reply, than: b.say)
    bubble = b.say == nil && b.choices.isEmpty && b.actions.isEmpty ? nil : b
    leaveAfterFarewell()
  }

  /// The bubble is closed: the bird flies back to its corner.
  func dismissBubble() {
    bubble = nil
    clearHighlight()
  }

  /// The offer of a demo: either answer is kept, so it is never asked again.
  func answerDemoOffer() {
    demoAnswered = true
    asksAgain = false
    advance(["kind": "offered"])
  }

  /// A demo picked from the first questions: the bird flies to its card in the
  /// demo list and offers to open it (the app says it; no model).
  func pickDemo(_ d: DemoEntry) {
    answerDemoOffer()
    speakingChat = nil
    let say = d.field == nil
      ? String(localized: "「\(d.title)」です。Card の形だけができていて、中身を AI が書きます。開いたら、チャットのツグミと一緒に実装させてみましょう。")
      : String(localized: "「\(d.title)」です。開くと、チャットのツグミが Card の動きと結果を一緒に見ていきます。")
    show(GuideShow(highlight: "demo:\(d.id)", say: say, choices: [Self.otherDemos],
                   actions: [GuideAction(kind: .openDemo, label: String(localized: "このデモを開く"), demo: d.id)]))
    bubble?.local = true
  }

  /// Ask for a Recommendation in a field: the guide picks one of its demos.
  func recommend(in field: String) {
    answerDemoOffer()
    demosRequested = true
    ask(String(localized: "「\(field)」のおすすめは？"), screen: .demos, session: nil, tab: nil,
        note: "[Interest] The user asks which demo of the field \"\(field)\" to see; pick one of its demos.")
  }

  func toggleFromMenu() {
    guard available else { return }
    open.toggle()
  }

  // MARK: - Conversation

  private func makeAgent() -> Agent? {
    if let agent { return agent }
    let a = Agent(id: "guide", transport: GuideTransport(), provider: AgentSettings.current(), role: .guide)
    Task {
      await a.setApprover { asked in
        guard let session = await GuideController.shared.readSession else { return [] }
        return await AppModel.shared.chat(for: session).approve(asked)
      }
    }
    agent = a
    return a
  }

  /// What the user is interested in (the first question on Home): the guide
  /// picks a demo for it and flies to it in the demo list.
  func answerInterest(_ text: String) {
    let text = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !text.isEmpty else { return }
    answerDemoOffer()
    demosRequested = true
    if scripted {
      advance(["kind": "interest", "text": text], asked: text)
      return
    }
    ask(text, screen: .demos, session: nil, tab: nil, note: "[Interest] The user answered what interests them; pick a demo for it.")
  }

  /// A question of the user, with what their window shows.
  func ask(_ text: String, screen: Screen, session: ProjectSession?, tab: WorkflowTab?, note: String? = nil) {
    let text = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard available, !busy, !text.isEmpty else { return }
    if scripted {
      answerScripted(text, chat: nil)
      return
    }
    guard let agent = makeAgent() else { return }
    readSession = session
    items.append(Item(role: "user", text: text))
    let prompt = text
    begin()
    Task {
      await agent.setProvider(await AgentSettings.guide())
      await agent.send(prompt: prompt, title: text, display: text) { e in Task { @MainActor in self.handle(e) } }
    }
  }

  /// A core-selected note for the model-backed guide; notes that arrive
  /// while it answers wait for the answer to end.
  private func tell(_ prompt: String, display: String, session: ProjectSession?) {
    // In the demo's copy, Tsugumi of its chat leads the tour.
    if let session {
      AppModel.shared.chat(for: session).tell(prompt: prompt, display: display)
      return
    }
    guard available, let agent = makeAgent() else { return }
    if busy {
      queued.append((prompt, display))
      return
    }
    if let session { readSession = session }
    items.append(Item(role: "note", text: display))
    let full = display
    begin()
    Task {
      await agent.setProvider(await AgentSettings.guide())
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
    guard connected, !busy, let agent = makeAgent() else { return }
    begin()
    Task {
      await agent.setProvider(await AgentSettings.guide())
      await agent.retry { e in Task { @MainActor in self.handle(e) } }
    }
  }

  /// A new conversation (the old one is gone; the tour stays).
  func newConversation() {
    guard !busy else { return }
    let a = agent
    Task { try? await a?.reset() }
    items = []
    status = nil
    failed = false
    clearHighlight()
    bubble = nil
  }

  private func begin() {
    busy = true
    failed = false
    status = nil
    pending = []
    pendingChoices = []
    activity = String(localized: "考えています")
    // The bird stays where it is and thinks.
    bubble = GuideBubble(target: highlight, thinking: true)
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
    case .toolInput:
      break
    case .toolStarted(_, let name, let args):
      if name == "guide_show" {
        speakingChat = nil
        let shown = GuideAction.parse(args)
        show(shown)
        pending += shown.actions
        pendingChoices += shown.choices
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
    case .compacting, .compacted, .helper:
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
      bubble = nil
      queued = []
      failed = s != "cancelled"
      status = s == "cancelled" ? String(localized: "回答を止めました。") : String(localized: "うまく答えられませんでした: \(s)")
    }
  }

  private func ended() {
    busy = false
    activity = nil
    // The bird says the answer's first sentence where it is, when guide_show gave no line.
    let reply = items.last.flatMap { $0.role == "assistant" ? $0.text : nil } ?? ""
    if var b = bubble {
      if b.thinking { b = GuideBubble(target: b.target) }
      if b.say == nil { b.say = Self.firstSentence(reply) }
      // On Home the bubble is the conversation: it points nowhere else.
      bubble = b.say == nil && b.choices.isEmpty && b.actions.isEmpty && b.target == nil ? nil : b
      leaveAfterFarewell()
    }
    guard !pending.isEmpty || !pendingChoices.isEmpty else { return }
    // Only the newest buttons and choices are offered.
    for i in items.indices { items[i].actions = []; items[i].choices = [] }
    if let i = items.lastIndex(where: { $0.role == "assistant" }), i == items.count - 1 {
      items[i].actions = pending
      items[i].choices = pendingChoices
    } else {
      items.append(Item(role: "assistant", text: "", actions: pending, choices: pendingChoices))
    }
    pending = []
    pendingChoices = []
  }

  /// The opening of a reply for the bubble: its first sentences, up to about 120 characters.
  nonisolated static func firstSentence(_ text: String) -> String? {
    let plain = text.replacingOccurrences(of: "**", with: "").replacingOccurrences(of: "`", with: "")
      .split(separator: "\n").map { $0.trimmingCharacters(in: CharacterSet(charactersIn: " #-•*>")) }.first { !$0.isEmpty } ?? ""
    guard !plain.isEmpty else { return nil }
    // Whole sentences while they fit, at least the first.
    var s = ""
    var rest = Substring(plain)
    while !rest.isEmpty {
      let end = rest.firstIndex { "。！？!?".contains($0) }.map { rest.index(after: $0) } ?? rest.endIndex
      let sentence = String(rest[..<end])
      if !s.isEmpty && s.count + sentence.count > 120 { break }
      s += sentence
      rest = rest[end...].drop { $0 == " " }
    }
    return s.count > 140 ? String(s.prefix(138)) + "…" : s
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
    var choices: [String] = []
    for m in conv.fullHistory {
      if m.role == "user" {
        let isNote = m.content.contains { if case .text(let t) = $0 { return t.hasPrefix("[Tour]") || t.hasPrefix("[Event]") }; return false }
        if let d = m.display {
          if !shown.isEmpty || !choices.isEmpty { out.append(Item(role: "assistant", text: "", actions: shown, choices: choices)) }
          shown = []
          choices = []
          out.append(Item(role: isNote ? "note" : "user", text: d))
        }
        continue
      }
      for b in m.content {
        switch b {
        case .text(let t):
          out.append(Item(role: "assistant", text: t, actions: shown, choices: choices))
          shown = []
          choices = []
        case .toolUse(_, let n, let a) where n == "guide_show":
          let g = GuideAction.parse(a)
          shown = g.actions
          choices = g.choices
        default: break
        }
      }
    }
    if !shown.isEmpty || !choices.isEmpty { out.append(Item(role: "assistant", text: "", actions: shown, choices: choices)) }
    if let newest = out.lastIndex(where: { !$0.actions.isEmpty || !$0.choices.isEmpty }) {
      for i in out.indices where i != newest { out[i].actions = []; out[i].choices = [] }
    }
    items = out
  }

  // MARK: - Without a model connection

  /// Presentation snapshots; no transition table or progress persistence lives here.
  @ObservationIgnored private var guideStep = ""
  @ObservationIgnored private var pendingGuide: Task<Void, Never>?

  private func advance(_ event: [String: Any], asked: String? = nil, chat: ChatController? = nil) {
    let previous = pendingGuide
    var event = event
    event["scripted"] = scripted
    pendingGuide = Task {
      await previous?.value
      do {
        let r = try await CoreAgentClient.shared.call("GuideAdvanceRequest", [
          "session_id": "desktop", "language": AppPreferences.Language.running == .en ? "en" : "ja", "event": event])
        setupDone = r["setupDone"] as? Bool ?? false
        demoAnswered = r["demoAnswered"] as? Bool ?? false
        guideStep = r["step"] as? String ?? ""
        if let t = r["tour"] as? [String: Any], let demo = t["demo"] as? String,
           let title = t["title"] as? String, let path = t["path"] as? String {
          let next = Tour(demo: demo, title: title, path: path, started: t["started"] as? Bool ?? false)
          if tour?.demo != next.demo || tour?.path != next.path || tour?.started != next.started { tour = next }
        } else if tour != nil { tour = nil }
        let display = asked ?? (r["asked"] as? String).flatMap { $0.isEmpty ? nil : $0 }
        if r["present"] as? Bool == true {
          let data = try JSONSerialization.data(withJSONObject: r)
          let shown = GuideAction.parse(String(decoding: data, as: UTF8.self))
          present(r["text"] as? String ?? "", shown: shown, asked: display, chat: chat)
        }
        if !scripted, let note = r["note"] as? String, !note.isEmpty, let chat {
          let prompt: String
          switch note {
          case "start": prompt = Agent.guideText("tourStart", ["demo": tour?.demo ?? "", "title": tour?.title ?? ""])
          case "resume": prompt = Agent.guideText("tourResume", ["demo": tour?.demo ?? ""])
          default:
            let detail = note == "run" ? Agent.guideText("runEnded", ["run": event["run"] as? String ?? "-", "state": event["state"] as? String ?? "none"])
              : note == "disconnected" ? Agent.guideText("disconnected")
              : Agent.guideText("reconnected", ["state": event["state"] as? String ?? "none"])
            prompt = Agent.guideText("event", ["event": detail])
          }
          tell(prompt, display: display ?? "", session: chat.session)
        }
      } catch {
        status = String(describing: error)
        failed = true
      }
    }
  }

  func actionFinished(_ action: String, project: String? = nil) {
    var event: [String: Any] = ["kind": "action_finished", "text": action]
    event["project"] = project
    advance(event)
  }

  func answerScripted(_ text: String, chat: ChatController?) {
    var event: [String: Any] = ["kind": "message", "text": text, "expected_step": guideStep]
    if let s = chat?.session {
      event["project"] = s.identity.url.path; event["title"] = s.scopeName([])
      event["connected"] = s.connected; event["active"] = s.runActive
      event["run"] = s.currentRunId ?? "-"
      event["state"] = s.runStatus.map { String(describing: $0.state) } ?? "none"
    }
    advance(event, asked: text, chat: chat)
  }

  private func present(_ text: String, shown: GuideShow, asked: String?, chat: ChatController?) {
    if let chat {
      chat.showScripted(asked: asked, reply: text, shown: shown)
      guard touring(chat.session) || shown.endTour else { return }
      speakingChat = chat
    } else {
      speakingChat = nil
      for i in items.indices { items[i].actions = []; items[i].choices = [] }
      if let asked { items.append(Item(role: "user", text: asked)) }
      items.append(Item(role: "assistant", text: text, actions: shown.actions, choices: shown.choices))
    }
    show(shown)
    bubble?.more = Self.saysMore(text, than: shown.say)
    leaveAfterFarewell()
  }

  // MARK: - Pointing

  /// Where the bird flies and the ring is drawn, until the bubble is closed or it moves on.
  func point(at target: String?) {
    highlightTask?.cancel()
    highlight = target
    // A demo's card is in Home's demo list: it opens to show it.
    if target?.hasPrefix("demo:") == true { demosRequested = true }
    // A Card inside a Card in Card: its workflow opens on the canvas.
    if let t = target, t.hasPrefix("card:") {
      let parts = t.dropFirst(5).split(separator: "/").map(String.init)
      if parts.count > 1 { scopeRequest = Array(parts.dropLast()) }
    }
  }

  func clearHighlight() {
    highlightTask?.cancel()
    highlight = nil
  }

  /// Whether a project is the copy of the demo being shown.
  func touring(_ s: ProjectSession) -> Bool { tour.map { Self.same($0.path, s.identity.url) } ?? false }

  /// Whether the guide points at `target` (the run button is also "stop";
  /// "card:<id>" is also that Card inside the Card in Card on the screen).
  func points(at target: String) -> Bool {
    guard let h = highlight else { return false }
    if h == target || (target == "run" && h == "stop") { return true }
    return h.hasPrefix("card:") && !h.dropFirst(5).contains("/") && target.hasPrefix("card:") && target.hasSuffix("/" + h.dropFirst(5))
  }

  nonisolated static func same(_ path: String, _ url: URL) -> Bool {
    URL(fileURLWithPath: path).resolvingSymlinksInPath().standardizedFileURL.path == url.resolvingSymlinksInPath().standardizedFileURL.path
  }

  // MARK: - Demo tours

  /// The user chose a demo: its copy is about to open; the tour starts there.
  func startTour(_ demo: DemoEntry, copy: URL) {
    asksAgain = false
    observed = nil
    open = false
    advance(["kind": "start", "demo": demo.id, "title": demo.title, "project": copy.path])
  }

  func endTour() {
    observed = nil
    advance(["kind": "end"])
  }

  func resumeTour() -> URL? {
    guard let t = tour, FileManager.default.fileExists(atPath: URL(fileURLWithPath: t.path).appendingPathComponent("tsugumi.json").path) else {
      endTour()
      return nil
    }
    resumeAsked = true
    observed = nil
    return URL(fileURLWithPath: t.path)
  }

  @ObservationIgnored private var resumeAsked = false

  /// Report screen facts once per change; agentd decides whether to speak.
  func observe(_ s: ProjectSession, tab: WorkflowTab?) {
    guard available, let t = tour, Self.same(t.path, s.identity.url) else { return }
    tourTab = tab
    let snapshot = "\(s.identity.url.path):\(runKey(s)):\(s.runActive):\(t.started)"
    guard snapshot != observed || resumeAsked else { return }
    observed = snapshot
    let resume = resumeAsked && s.connected
    if resume { resumeAsked = false }
    advance(["kind": resume ? "resume" : "observe", "project": s.identity.url.path, "title": t.title,
             "connected": s.connected, "active": s.runActive, "run": s.currentRunId ?? "-",
             "state": s.runStatus.map { String(describing: $0.state) } ?? "none"], chat: AppModel.shared.chat(for: s))
  }

  /// The run as the guide follows it: "offline", or "<run id>:<state>".
  private func runKey(_ s: ProjectSession) -> String {
    guard s.connected else { return "offline" }
    return "\(s.currentRunId ?? "-"):\(s.runStatus.map { String(describing: $0.state) } ?? "none")"
  }
}
