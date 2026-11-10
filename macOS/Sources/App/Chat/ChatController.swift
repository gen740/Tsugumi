import FlatBuffers
import Foundation
import Observation
import SwiftUI
import TsugumiAgent
import TsugumiLauncher
import TsugumiRPC

/// Bridges the Agent's tool calls to this project's authenticated RPC client.
/// The Agent edits documents only through core RPC, never through UI state.
struct RPCToolTransport: ToolTransport {
  let session: ProjectSession
  let replyNames: [UInt8: String]

  func call(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset) async throws -> (replyType: String, table: FlatBuffers.Table) {
    guard let rpc = await session.rpc else { throw RPCError.disconnected }
    let (type, table) = try await rpc.callRaw(payloadType: payloadType, build: build)
    return (replyNames[type] ?? "Unknown", table)
  }
}

/// A skill: instructions attached to a message with `$name`.
struct Skill: Identifiable, Hashable {
  var id: String { path }
  let name: String
  let description: String
  let path: String
  let content: String
  let source: String
}

/// Reads skills from a folder: `<name>/SKILL.md` or `<name>.md`.
func loadSkills(in folder: URL, source: String) -> [Skill] {
  let fm = FileManager.default
  guard let items = try? fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.isDirectoryKey]) else { return [] }
  var out: [Skill] = []
  for item in items.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
    var file = item
    var name = item.deletingPathExtension().lastPathComponent
    if (try? item.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
      file = item.appendingPathComponent("SKILL.md")
      name = item.lastPathComponent
    } else if item.pathExtension.lowercased() != "md" {
      continue
    }
    guard let data = try? Data(contentsOf: file), data.count < 512 * 1024 else { continue }
    let text = String(decoding: data, as: UTF8.self)
    var description = ""
    var lines = text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
    if lines.first == "---", let end = lines.dropFirst().firstIndex(of: "---") {
      for l in lines[1..<end] {
        if l.hasPrefix("name:") { name = l.dropFirst(5).trimmingCharacters(in: .whitespaces) }
        if l.hasPrefix("description:") { description = l.dropFirst(12).trimmingCharacters(in: .whitespaces) }
      }
      lines = Array(lines[(end + 1)...])
    }
    if description.isEmpty {
      description = lines.first { !$0.trimmingCharacters(in: .whitespaces).isEmpty }?
        .trimmingCharacters(in: CharacterSet(charactersIn: "# ")) ?? ""
    }
    let clean = String(name.map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" ? $0 : "-" })
    out.append(Skill(name: clean, description: description, path: file.path, content: text, source: source))
  }
  return out
}

/// The chat of one project: several conversations (sessions), each an Agent
/// with its own persisted transcript.
@MainActor
@Observable
final class ChatController {
  struct Item: Identifiable {
    let id = UUID()
    var role: String
    var text: String
    var tool: (name: String, ok: Bool?)?
    var archived = false
    /// A helper's row (role "helper"): its name, its current or last tool
    /// call (text: the arguments) and how its work ended (nil: working).
    var helper: String?
    var ended: String?
  }

  struct SessionInfo: Identifiable, Hashable {
    let id: String
    var title: String
    var updatedAt: Date
    var messages: Int
  }

  enum Reference: Hashable, Identifiable {
    case card(CardKey)
    case display(CardKey)
    var id: String { label }
    var label: String {
      switch self {
      case .card(let k): return "Card \(k)"
      case .display(let k): return String(localized: "可視化 \(k)")
      }
    }
  }

  struct SearchHit: Identifiable {
    let id = UUID()
    let session: String
    let title: String
    let snippet: String
  }

  let session: ProjectSession
  /// "思考の過程を描く": the Agent's thinking in one line for the canvas.
  let thoughts: ThoughtSummarizer
  private(set) var items: [Item] = []
  /// The last turn stopped at the step limit and can go on.
  private(set) var stoppedAtLimit = false

  /// Shows a sample exchange without calling a model (UI script `chatsample`).
  func showSample(question: String, answer: String) {
    items.append(Item(role: "user", text: question))
    items.append(Item(role: "assistant", text: answer))
  }
  private(set) var busy = false
  private(set) var status: String?
  private(set) var failed = false
  /// The last turn stopped at the ChatGPT plan's usage limit ("Manage usage").
  private(set) var usageLimited = false
  /// The last turn could not make the conversation fit the model's context.
  private(set) var contextFull = false
  private(set) var sessions: [SessionInfo] = []
  private(set) var currentId: String = ""
  private(set) var usage = Usage()
  private(set) var skills: [Skill] = []
  var references: [Reference] = []
  /// Folders of the Mac attached to this chat: shared read-only with the
  /// project's environment, read by the Agent (attached_folder_read) only.
  private(set) var folders: [String] = []
  private(set) var attaching = false

  /// Attaches a folder; the environment starts again to share it.
  func attach(folder url: URL) async {
    let path = url.standardizedFileURL.path
    guard !folders.contains(path) else { return }
    attaching = true
    defer { attaching = false }
    folders.append(path)
    await agent?.setFolders(folders)
    await session.environments.setExtraFolders(session.identity, folders)
    await session.restartEnvironment()
  }

  /// No longer offered to the Agent, which refuses it from now on (the share
  /// itself goes when the environment next starts).
  func detach(folder path: String) {
    folders.removeAll { $0 == path }
    let (agent, folders) = (self.agent, self.folders)
    Task {
      await agent?.setFolders(folders)
      await session.environments.setExtraFolders(session.identity, folders)
    }
  }
  private(set) var available = true
  @ObservationIgnored private var agent: Agent?
  /// Opening a conversation, until its unfinished work (if any) has resumed.
  @ObservationIgnored private var resuming: Task<Void, Never>?
  @ObservationIgnored private var catalog: ToolCatalog?
  @ObservationIgnored private var replyNames: [UInt8: String] = [:]
  @ObservationIgnored private let dir: URL

  @ObservationIgnored private let connection: @MainActor () -> ProviderConfig
  var provider: ProviderConfig { connection() }

  /// The references the Agent may read (reference_read): the bundled demos
  /// that are not Draft tasks or their answers (Agent.referenceDemos), the
  /// Python SDK and the Card guide; next to the repository in a
  /// development build.
  static let references: [String: URL] = {
    let fm = FileManager.default
    if let res = Bundle.main.resourceURL, fm.fileExists(atPath: res.appendingPathComponent("reference/card-guide.md").path) {
      return Agent.referenceDemos(in: res.appendingPathComponent("demos/\(AppPreferences.Language.running.rawValue)", isDirectory: true)).merging([
        "sdk/python/tsugumi/__init__.py": res.appendingPathComponent("reference/tsugumi-sdk.py"),
        "card-guide.md": res.appendingPathComponent("reference/card-guide.md")]) { a, _ in a }
    }
    #if DEBUG
    // A development build run from the source tree reads the repository.
    let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../..").standardizedFileURL
    return Agent.referenceDemos(in: repo.appendingPathComponent("demos/\(AppPreferences.Language.running.rawValue)", isDirectory: true)).merging([
      "sdk/python/tsugumi/__init__.py": repo.appendingPathComponent("sdk/python/tsugumi/__init__.py"),
      "card-guide.md": repo.appendingPathComponent("agent/Reference/card-guide.md")]) { a, _ in a }
    #else
    return [:]
    #endif
  }()

  /// `directory` and `connection`: where its conversations are and what it
  /// answers with, instead of the app's (tests).
  init(session: ProjectSession, directory: URL? = nil, connection: @escaping @MainActor () -> ProviderConfig = AgentSettings.chat) {
    self.session = session
    self.connection = connection
    thoughts = ThoughtSummarizer(presence: session.presence)
    dir = directory ?? AppChannel.current.supportDirectory.appendingPathComponent("agent/\(session.identity.id)", isDirectory: true)
    guard let bfbs = RPCSchema.bfbs(), let schema = try? BFBS(data: bfbs), let catalog = try? ToolCatalog(schema: schema) else {
      status = String(localized: "RPC スキーマを読み込めないため、チャットは使えません。通常の操作はすべて UI から行えます。")
      available = false
      return
    }
    self.catalog = catalog
    if let e = schema.enums.first(where: { $0.name == "Tsugumi.RPC.ReplyPayload" }) {
      for v in e.values { replyNames[UInt8(v.value)] = v.name }
    }
    refreshSessions()
    select(sessions.first?.id ?? UUID().uuidString.lowercased())
    reloadSkills()
  }

  // MARK: - Sessions

  private func url(_ id: String) -> URL { dir.appendingPathComponent("sessions/\(id).json") }

  /// The saved sessions, newest first.
  func refreshSessions() {
    let files = (try? FileManager.default.contentsOfDirectory(at: dir.appendingPathComponent("sessions"), includingPropertiesForKeys: nil)) ?? []
    sessions = files.filter { $0.pathExtension == "json" }.map { f in
      guard let c = try? Conversation.read(f) else {
        return SessionInfo(id: f.deletingPathExtension().lastPathComponent, title: f.lastPathComponent,
                           updatedAt: .distantPast, messages: 0)
      }
      return SessionInfo(id: f.deletingPathExtension().lastPathComponent, title: c.title ?? firstText(c) ?? String(localized: "新しい会話"),
                         updatedAt: c.updatedAt, messages: c.fullHistory.count)
    }
    .sorted { $0.updatedAt > $1.updatedAt }
  }

  private func firstText(_ c: Conversation) -> String? {
    for m in c.fullHistory where m.role == "user" {
      if let d = m.display { return String(d.prefix(60)) }
      for b in m.content { if case .text(let t) = b { return String(t.prefix(60)) } }
    }
    return nil
  }

  func select(_ id: String) {
    guard let catalog else { return }
    // The conversation left behind stops first: it changes nothing after the
    // new one starts (the new one waits for it).
    let previous = agent
    if let previous { Task { await previous.cancel() } }
    resuming?.cancel()
    resuming = nil
    answer += 1
    busy = false
    failed = false
    usageLimited = false
    status = nil
    demoMode = false
    currentId = id
    let agent: Agent
    do {
      agent = try Agent(catalog: catalog, transport: RPCToolTransport(session: session, replyNames: replyNames),
                      provider: provider, storage: url(id), references: Self.references)
    } catch {
      self.agent = nil
      items = []
      failed = true
      status = String(describing: error)
      return
    }
    self.agent = agent
    items = []
    // (the approver holds the controller weakly: the Agent outlives no window)
    let approver: Agent.Approver = { [weak self] asked in await self?.approve(asked) ?? [] }
    resuming = Task {
      await previous?.cancelAndWait()
      await agent.setApprover(approver)
      await agent.setFolders(self.folders)
      let conv = await agent.conversation
      guard !Task.isCancelled else { return }
      usage = conv.usage
      load(conv)
      await resumeWhenConnected(agent)
    }
  }

  /// Resumes work left unfinished by a disconnect, a restart or leaving the
  /// project, as any answer goes: the chat shows it working and it can be
  /// stopped. Its tools need the project, so it starts once that is connected
  /// (until then nothing can be sent either).
  private func resumeWhenConnected(_ agent: Agent) async {
    guard await agent.hasUnfinishedWork else { return }
    while !session.connected {
      if session.closed { return }
      try? await Task.sleep(for: .milliseconds(100))
      if Task.isCancelled { return }
    }
    guard !Task.isCancelled, !busy, await agent.hasUnfinishedWork else { return }
    let events = begin()
    await agent.setProvider(provider)
    await agent.resume(events: events)
  }

  private func load(_ conv: Conversation) {
    var out: [Item] = []
    for (i, m) in conv.fullHistory.enumerated() {
      let archived = i < conv.archive.count
      if let d = m.display {
        out.append(Item(role: m.role, text: d, archived: archived))
        continue
      }
      for b in m.content {
        switch b {
        case .text(let t): out.append(Item(role: m.role, text: t, archived: archived))
        case .toolUse(_, let n, let a): out.append(Item(role: "tool", text: a, tool: (n, nil), archived: archived))
        case .toolResult(_, let c, let e, _):
          if let j = out.lastIndex(where: { $0.role == "tool" && $0.tool?.ok == nil }) {
            out[j].tool?.ok = !e
            out[j].text += "\n→ " + Agent.toolSummary(c)
          }
        case .webSearch(let s):
          out.append(Item(role: "tool", text: s.arguments + "\n→ " + s.summary, tool: ("web_search", s.error == nil), archived: archived))
        }
      }
    }
    items = out
  }

  func newSession() {
    select(UUID().uuidString.lowercased())
    refreshSessions()
  }

  func delete(_ id: String) {
    try? FileManager.default.removeItem(at: url(id))
    // (and its helpers' conversations, from delegate)
    try? FileManager.default.removeItem(at: url(id).deletingPathExtension().appendingPathExtension("helpers"))
    refreshSessions()
    if id == currentId { select(sessions.first?.id ?? UUID().uuidString.lowercased()) }
  }

  /// Full-text search over every session, compacted parts included.
  func search(_ text: String) -> [SearchHit] {
    guard !text.isEmpty else { return [] }
    var hits: [SearchHit] = []
    for s in sessions {
      guard let c = try? Conversation.read(url(s.id)) else { continue }
      for m in c.fullHistory {
        for b in m.content {
          guard case .text(let t) = b, let r = t.range(of: text, options: [.caseInsensitive, .diacriticInsensitive]) else { continue }
          let lo = t.index(r.lowerBound, offsetBy: -40, limitedBy: t.startIndex) ?? t.startIndex
          let hi = t.index(r.upperBound, offsetBy: 60, limitedBy: t.endIndex) ?? t.endIndex
          hits.append(SearchHit(session: s.id, title: s.title, snippet: String(t[lo..<hi]).replacingOccurrences(of: "\n", with: " ")))
        }
      }
    }
    return hits
  }

  // MARK: - Skills and references

  /// The built-in $demo: Tsugumi of this chat shows the screen around (it flies only then).
  static let demoSkill = Skill(name: "demo", description: String(localized: "ツグミが画面を飛び回って、デモや画面の使い方を案内します"),
                               path: "builtin:demo", content: Agent.demoSkill, source: "Tsugumi")

  func reloadSkills() {
    var out = [Self.demoSkill]
    out += loadSkills(in: session.identity.url.appendingPathComponent("skills"), source: String(localized: "プロジェクト"))
    for f in AppPreferences.shared.skillFolders where f.enabled {
      out += loadSkills(in: URL(fileURLWithPath: f.path), source: URL(fileURLWithPath: f.path).lastPathComponent)
    }
    skills = out
  }

  func addReference(_ r: Reference) {
    if !references.contains(r) { references.append(r) }
  }

  private func describe(_ r: Reference) -> String {
    switch r {
    case .card(let k):
      var s = "Card \(k) (\(session.kind(k).label), definition \(session.definitionDir(k) ?? "-"), name \"\(session.displayName(k))\""
      if let st = session.cardStates[k] { s += ", last state \(st.state.label)" }
      if let r = session.resultSummary(k) { s += ", result: \(r)" }
      return s + ")"
    case .display(let k):
      switch session.displays[k]?.value {
      case .text(let t)?: return "Display of Card \(k) (text):\n\(t.prefix(4000))"
      case .markdown(let t)?: return "Display of Card \(k) (Markdown):\n\(t.prefix(4000))"
      case .scalar(let v, let l)?: return "Display of Card \(k): \(l) = \(v)"
      case .lineChart(let series)?:
        let lines = series.map { s in
          let pts = zip(s.x, s.y).prefix(200).map { "(\($0), \($1))" }.joined(separator: " ")
          return "series \(s.label): \(pts)"
        }
        return "Display of Card \(k) (line chart):\n" + lines.joined(separator: "\n")
      case .table(let cols, let rows)?:
        return "Display of Card \(k) (table):\n" + ([cols] + rows.prefix(200)).map { $0.joined(separator: "\t") }.joined(separator: "\n")
      case .image(let type, let data, let alt)?:
        return "Display of Card \(k): \(type) image, \(data.count) bytes, \(alt)"
      case .points(let x, _, _, _)?:
        return "Display of Card \(k): 3D point cloud with \(x.count) points"
      case .module(let name, let json)?:
        return "Display of Card \(k) (module \(name)):\n\(json.prefix(4000))"
      case nil: return "Display of Card \(k): no result yet"
      }
    }
  }

  /// `$name` tokens that match a skill.
  func skills(in text: String) -> [Skill] {
    let names = Set(text.split(whereSeparator: { $0.isWhitespace }).filter { $0.hasPrefix("$") }.map { String($0.dropFirst()) })
    return skills.filter { names.contains($0.name) }
  }

  // MARK: - Conversation

  func send(_ text: String, selection: [CardKey] = []) {
    guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
    // No model connection: Tsugumi answers from its script (a connection of this chat's own, as in tests, is used).
    if GuideController.shared.scripted, provider.auth != .none, !busy {
      GuideController.shared.answerScripted(text, chat: self)
      return
    }
    guard let agent else { return }
    var prompt = text
    let used = skills(in: text)
    if used.contains(where: { $0.name == "demo" }) { demoMode = true }
    for s in used { prompt += "\n\n[Skill \(s.name)]\n\(s.content)" }
    let refs = references + selection.filter { k in !references.contains(.card(k)) }.map { Reference.card($0) }
    if !refs.isEmpty { prompt += "\n\n[References]\n" + refs.map(describe).joined(separator: "\n") }
    if !folders.isEmpty {
      prompt += "\n\n[Attached folders]\n" + folders.map { "\($0) (read with attached_folder_read, folder: \"\($0)\")" }.joined(separator: "\n")
    }
    let shown = text + (refs.isEmpty ? "" : "\n📎 " + refs.map(\.label).joined(separator: ", "))
    items.append(Item(role: "user", text: shown))
    references = []
    let events = begin()
    Task {
      await agent.setProvider(provider)
      // The chat shows what was typed, not the skills and references attached to it.
      await agent.send(prompt: prompt, title: text, display: shown, events: events)
    }
  }

  /// Demo mode ($demo, or a demo tour): the bird leads the user about the
  /// screen for this chat; it ends with the tour's farewell. Otherwise the chat
  /// is a chat, whose bird flies only to show where a feature asked about is.
  private(set) var demoMode = false

  /// Replies to tap and buttons Tsugumi offered with its last answer (guide_show).
  private(set) var choices: [String] = []
  private(set) var offered: [GuideAction] = []
  /// Without a model connection: Tsugumi's fixed reply (GuideScript) to what
  /// was asked or what the app noted, with its choices and buttons.
  func showScripted(asked: String?, reply: String, shown: GuideShow) {
    if let asked { items.append(Item(role: "user", text: asked)) }
    items.append(Item(role: "assistant", text: reply))
    choices = shown.choices
    offered = shown.actions
  }

  /// Notes of the app (a demo tour's runs) that came while it answered.
  @ObservationIgnored private var notes: [(prompt: String, display: String)] = []

  /// Something the app tells Tsugumi (a tour step, a run that ended); one that
  /// comes while it answers waits for the answer to end.
  func tell(prompt: String, display: String) {
    // A tour is demo mode: the skill comes with its first note.
    var prompt = prompt
    if !demoMode {
      prompt += "\n\n[Skill demo]\n" + Agent.demoSkill
      demoMode = true
    }
    if busy {
      notes.append((prompt, display))
      return
    }
    sendRequest(display: display, prompt: prompt)
  }

  private func flushNotes() {
    guard !notes.isEmpty else { return }
    let n = notes
    notes = []
    sendRequest(display: n.last!.display, prompt: n.map(\.prompt).joined(separator: "\n"))
  }

  private var lastReply: String { items.last { $0.role == "assistant" }?.text ?? "" }

  /// Sends a request the app wrote for the model; the chat shows `display`.
  private func sendRequest(display: String, prompt: String) {
    guard let agent else { return }
    items.append(Item(role: "user", text: display))
    let events = begin()
    Task {
      await agent.setProvider(provider)
      await agent.send(prompt: prompt, title: display.components(separatedBy: "\n")[0], display: display, events: events)
    }
  }

  /// Asks the Agent to implement the Draft Cards of a scope (after they were wired and saved).
  func generateDrafts(scope: [String], cards: [CardKey]) {
    let items = cards.map { k in
      // A Card's definition folder is named by its id.
      let meta = session.cardMeta(k)
      return (key: k, definition: path(k).last ?? "-", description: meta?["description"]?.string ?? "",
              content: meta?["content"]?.string ?? "", context: meta?["context"]?.string ?? "")
    }
    let names = cards.map { path($0).last ?? $0 }.joined(separator: "、")
    sendRequest(display: String(localized: "Draft \(cards.count) 枚（\(names)）から Card を生成します"),
                prompt: Agent.draftRequest(scope: scope, cards: items))
  }

  /// Asks the Agent to do what the open comments on these Cards ask.
  func generateFromComments(scope: [String], cards: [CardKey]) {
    let items = cards.map { k in
      (key: k, definition: path(k).last ?? "-", comments: session.openComments(k).map { (id: $0.id, text: $0.text) })
    }
    let count = items.reduce(0) { $0 + $1.comments.count }
    let asked = items.flatMap { c in c.comments.map { "\(c.definition): \($0.text)" } }
    sendRequest(display: ([String(localized: "コメント \(count) 件に従って Card を生成します")] + asked).joined(separator: "\n"),
                prompt: Agent.commentRequest(scope: scope, cards: items))
  }

  // MARK: - Card permissions

  /// A save of the Agent waiting for the user: what it would give Cards.
  struct PermissionPrompt: Identifiable {
    let id = UUID()
    let requests: [CardPermission.Request]
    let answer: (Set<String>) -> Void
  }
  var permissionPrompt: PermissionPrompt?

  /// The widenings the user approves: those of the kinds allowed without
  /// asking at once, the others when asked (nothing when declined).
  func approve(_ asked: [String]) async -> Set<String> {
    let allowed = AppPreferences.shared.allowedPermissions
    let requests = asked.compactMap(CardPermission.Request.init)
    let approved = Set(requests.filter { allowed.contains($0.kind.rawValue) }.map(\.raw))
    let ask = requests.filter { !approved.contains($0.raw) }
    if ask.isEmpty { return approved }
    permissionPrompt?.answer([])
    let answer: Set<String> = await withCheckedContinuation { c in
      permissionPrompt = PermissionPrompt(requests: ask) { c.resume(returning: $0) }
    }
    return approved.union(answer)
  }

  /// The user's answer; `always` are kinds allowed without asking from now on.
  func answerPermissions(allow: Bool, always: Set<CardPermission>) {
    guard let p = permissionPrompt else { return }
    permissionPrompt = nil
    if allow { AppPreferences.shared.allowedPermissions.formUnion(always.map(\.rawValue)) }
    p.answer(allow ? Set(p.requests.map(\.raw)) : [])
  }

  func cancel() {
    if let p = permissionPrompt {
      permissionPrompt = nil
      p.answer([])
    }
    resuming?.cancel()
    resuming = nil
    Task { await agent?.cancel() }
    busy = false
    session.presence.turnEnded(.stopped)
    status = String(localized: "回答を停止しました。受理済みの実行は止まりません（実行の停止は Canvas の「停止」）。")
  }

  /// Goes on after the step limit stopped the Agent.
  func continueWork() {
    guard let agent else { return }
    items.append(Item(role: "user", text: String(localized: "（止まったところから続けます）")))
    let events = begin()
    Task {
      await agent.setProvider(provider)
      await agent.continueAfterLimit(events: events)
    }
  }

  func retry() {
    guard let agent else { return }
    let events = begin()
    Task {
      await agent.setProvider(provider)
      await agent.retry(events: events)
    }
  }

  func compact() {
    guard let agent else { return }
    let events = begin()
    status = String(localized: "会話を圧縮しています…")
    Task {
      await agent.setProvider(provider)
      await agent.compact(events: events)
    }
  }

  /// An answer begins; what it reports goes to the returned closure.
  private func begin() -> @Sendable (AgentEvent) -> Void {
    answer += 1
    choices = []
    offered = []
    GuideController.shared.chatTurnBegan(self)
    busy = true
    failed = false
    usageLimited = false
    contextFull = false
    stoppedAtLimit = false
    status = nil
    session.presence.name = { [weak session] k in session?.nodeData(k).title ?? k }
    session.presence.turnStarted()
    thoughts.turnStarted()
    let number = answer
    return { e in Task { @MainActor in self.handle(e, of: number) } }
  }

  /// The tsugumi on the canvas follows what the Agent looks at and runs.
  private func showOnCanvas(tool: String, arguments: String, bird: AgentPresence? = nil) {
    let bird = bird ?? session.presence
    // What was written is not on the screen once it runs.
    if tool == "run" { bird.stopShowingCode() }
    switch PresenceEvents.look(tool: tool, arguments: arguments) {
    case .visit(let p)?: if session.instance(key(p)) != nil { bird.enqueue(.visit(key(p))) }
    case .run(let p)?: if session.instance(key(p)) != nil { bird.enqueue(.run(key(p))) }
    case nil: break
    }
  }

  /// Blocks moved by tidying in this session (for checks).
  private(set) var tidied = 0
  private var tidyTask: Task<Void, Never>?

  /// While the Agent works, what its Cards show must not cover other Cards:
  /// a moment after each step (when new output frames have come), room is
  /// made where blocks overlap (LayoutTidy).
  private func tidySoon() {
    guard session.isEditable else { return }
    tidyTask?.cancel()
    tidyTask = Task { @MainActor [weak self] in
      try? await Task.sleep(for: .milliseconds(900))
      guard let self, !Task.isCancelled else { return }
      withAnimation(.easeInOut(duration: 0.6)) { self.tidied += self.session.tidy() }
    }
  }

  /// A delegate call: its helpers' birds come out, each with a row here.
  private func startHelpers(_ arguments: String) {
    guard let v = try? JSONValue.parse(arguments), let tasks = v["tasks"]?.array else { return }
    let list = tasks.compactMap { t -> (name: String, cards: [String])? in
      guard let n = t["name"]?.string else { return nil }
      return (n, t["cards"]?.array?.compactMap(\.string) ?? [])
    }
    session.startHelpers(list)
    for t in list { items.append(Item(role: "helper", text: "", helper: t.name)) }
  }

  /// What a helper does: its bird and its row.
  private func handleHelper(_ name: String, _ e: AgentEvent) {
    let bird = session.helperPresence(name)
    let row = items.lastIndex { $0.role == "helper" && $0.helper == name }
    switch e {
    case .toolInput(let index, let tool, let delta):
      bird.toolInput(index: index, tool: tool, delta: delta)
    case .text:
      bird.doing(String(localized: "考えています"))
    case .toolStarted(_, let tool, let args):
      // The thought line follows the helpers too (and tidies as they work).
      thoughts.note(tool: "\(name)/\(tool)", arguments: args)
      bird.doing(PresenceEvents.activity(tool: tool))
      showOnCanvas(tool: tool, arguments: args, bird: bird)
      if let row { items[row].tool = (tool, nil); items[row].text = args }
    case .toolFinished(_, let tool, let ok, let summary):
      thoughts.note(tool: "\(name)/\(tool)", ok: ok, summary: summary)
      if PresenceEvents.writesSources(tool: tool) { bird.toolInputEnded(ok: ok) }
      tidySoon()
      if let row, items[row].tool?.name == tool { items[row].tool?.ok = ok }
    case .finished:
      bird.turnEnded(.done)
      if let row { items[row].ended = String(localized: "終わりました") }
    case .stepLimit:
      bird.turnEnded(.limit)
      if let row { items[row].ended = String(localized: "手数の上限で止まりました") }
    case .error(let s):
      bird.turnEnded(s == "cancelled" ? .stopped : .failed)
      if let row { items[row].ended = s == "cancelled" ? String(localized: "止めました") : String(localized: "エラー: \(s)") }
    default:
      break
    }
  }

  /// The answer the chat shows. One stopped or left behind may still be ending:
  /// its text and its end are no longer this chat's (it goes on with the newer
  /// one, which would be shown as stopped). What its tools did still counts.
  @ObservationIgnored private var answer = 0

  private func handle(_ e: AgentEvent, of number: Int) {
    if number != answer {
      switch e {
      case .text, .discardText, .retrying, .compacting, .compacted, .error, .finished, .stepLimit: return
      default: break
      }
    }
    switch e {
    case .text(let t):
      thoughts.note(text: t)
      session.presence.doing(String(localized: "考えています"))
      if let last = items.last, last.role == "assistant", last.tool == nil {
        items[items.count - 1].text += t
      } else {
        items.append(Item(role: "assistant", text: t))
      }
    case .helper(let name, let event):
      handleHelper(name, event)
    case .toolInput(let index, let tool, let delta):
      session.presence.toolInput(index: index, tool: tool, delta: delta)
    case .toolStarted(_, "guide_show", let args):
      // The bird flies and speaks; the chat keeps the choices under the answer.
      let shown = GuideAction.parse(args)
      choices = shown.choices
      offered = shown.actions
      // The bird flies for it: in demo mode, and outside it to show where a
      // feature the user asked about is (the rules keep it to that).
      GuideController.shared.chatShow(self, arguments: args)
      if shown.endTour { demoMode = false }
    case .toolFinished(_, "guide_show", _, _):
      break
    case .toolStarted(_, let name, let args):
      items.append(Item(role: "tool", text: args, tool: (name, nil)))
      if name == "delegate" { startHelpers(args) }
      session.presence.doing(PresenceEvents.activity(tool: name))
      showOnCanvas(tool: name, arguments: args)
      thoughts.note(tool: name, arguments: args)
    case .toolFinished(_, let name, let ok, let summary):
      thoughts.note(tool: name, ok: ok, summary: summary)
      if PresenceEvents.writesSources(tool: name) { session.presence.toolInputEnded(ok: ok) }
      tidySoon()
      if let i = items.lastIndex(where: { $0.role == "tool" && $0.tool?.name == name && $0.tool?.ok == nil }) {
        items[i].tool = (name, ok)
        items[i].text += "\n→ " + summary
      }
    case .compacting:
      // The next reply waits for it (over a minute with a long conversation).
      status = String(localized: "会話を圧縮しています…")
    case .compacted:
      // Earlier messages were summarized for the model; the whole text stays
      // in the conversation, so nothing is announced.
      status = nil
      reloadCurrent()
    case .retrying(let s):
      // The reply is asked again from its beginning.
      session.presence.toolInputEnded(ok: false)
      status = String(localized: "再試行中: \(s)")
    case .discardText:
      // The reply is asked again: what it had streamed goes.
      session.presence.toolInputEnded(ok: false)
      if let last = items.last, last.role == "assistant", last.tool == nil { items.removeLast() }
    case .error(let s):
      GuideController.shared.chatTurnEnded(self, reply: lastReply)
      if s == "cancelled" { notes = [] }
      busy = false
      session.presence.turnEnded(s == "cancelled" ? .stopped : .failed)
      failed = s != "cancelled"
      usageLimited = s == ChatGPTError.usageLimitText
      contextFull = s == Agent.contextFullText
      status = s == "cancelled" ? String(localized: "回答を停止しました（Workflow の実行は止めていません）。")
        : usageLimited ? String(localized: "Usage limit reached: ChatGPT の設定で、プランかこのアプリの利用上限を確かめてください。")
        : contextFull ? String(localized: "この会話はモデルの文脈に収まらなくなりました。新しい会話を始めてください（この会話は一覧に残ります）。")
        : String(localized: "エラー: \(s)")
      refreshAfterTurn()
    case .finished:
      GuideController.shared.chatTurnEnded(self, reply: lastReply)
      busy = false
      // A note of this turn (a retry, the compaction) is no longer current.
      status = nil
      session.presence.turnEnded()
      refreshAfterTurn()
      tidySoon()
      flushNotes()
    case .stepLimit(let steps, let run):
      GuideController.shared.chatTurnEnded(self, reply: lastReply)
      busy = false
      session.presence.turnEnded(.limit)
      stoppedAtLimit = true
      status = String(localized: "\(steps) 手の上限で中断しました。「続ける」で続きから再開できます。")
        + (run.map { String(localized: " 実行 \(String($0.prefix(8))) の結果はまだ確かめていません。") } ?? "")
      refreshAfterTurn()
    }
  }

  private func reloadCurrent() {
    guard let agent else { return }
    Task {
      load(await agent.conversation)
      usage = await agent.conversation.usage
    }
  }

  private func refreshAfterTurn() {
    guard let agent else { return }
    Task {
      usage = await agent.conversation.usage
      refreshSessions()
    }
  }
}

enum AgentSettings {
  /// The small model Tsugumi speaks with on the ChatGPT plan (the Home guide,
  /// the canvas bird's thoughts): gpt-6.1-luna when the account offers it,
  /// else its newest luna ("agent.speechModel.chatgpt" overrides it).
  static var chatGPTSpeechModel: String {
    let d = UserDefaults.standard
    if let m = d.string(forKey: "agent.speechModel.chatgpt"), !m.isEmpty { return m }
    return speechModel(offered: d.stringArray(forKey: modelsKey("chatgpt")) ?? [], fallback: config(for: "chatgpt").model)
  }

  /// gpt-6.1-luna if offered, else the newest luna offered, else `fallback`
  /// (the chat's model, which the plan takes, when nothing is listed yet).
  nonisolated static func speechModel(offered: [String], fallback: String) -> String {
    let preferred = "gpt-6.1-luna"
    if offered.contains(preferred) { return preferred }
    let version = { (m: String) -> [Int] in m.split(whereSeparator: { !$0.isNumber }).compactMap { Int($0) } }
    return offered.filter { $0.contains("luna") }.max { version($0).lexicographicallyPrecedes(version($1)) } ?? fallback
  }

  /// The connection of the Home guide: the Agent's, with a small model, as for
  /// the canvas bird's thoughts: claude-haiku-5-5 on Anthropic, the speech model
  /// on the ChatGPT plan (OpenAI keeps the Agent's). Demo tours are led by the
  /// chat, with the chat's model.
  static func guide() -> ProviderConfig {
    var c = current()
    if c.auth == .chatGPT { c.model = chatGPTSpeechModel }
    else if c.auth == .apiKey, c.api == .anthropicMessages { c.model = "claude-haiku-5-5" }
    return c
  }

  /// The connection chosen in Settings > API ("anthropic", "openai", "chatgpt" or "fixture").
  static var provider: String { UserDefaults.standard.string(forKey: "agent.provider") ?? "anthropic" }

  static func current() -> ProviderConfig { config(for: provider) }

  /// The connection of the chat's Agent and its helpers: with the effort and
  /// the Fast mode chosen in the model menu. The guide, the summaries and the
  /// checks use `current()`, without them.
  static func chat() -> ProviderConfig {
    let d = UserDefaults.standard
    return current().choosing(effort: d.string(forKey: effortKey) ?? "", fastModel: d.string(forKey: fastModelKey) ?? "")
  }

  /// The model menu's effort ("" or none: the model's default), for every model that takes it.
  static let effortKey = "agent.effort"
  /// The one model Fast mode was turned on for.
  static let fastModelKey = "agent.fastModel"

  /// A connection with its chosen model (each keeps its own).
  static func config(for provider: String) -> ProviderConfig {
    let d = UserDefaults.standard
    switch provider {
    case "fixture":
      return .localFixture(port: d.integer(forKey: "agent.fixturePort") == 0 ? 8765 : d.integer(forKey: "agent.fixturePort"))
    case "chatgpt":
      var c = ProviderConfig.chatGPT
      if let m = d.string(forKey: "agent.model.chatgpt"), !m.isEmpty { c.model = m }
      c.webSearch = d.bool(forKey: webSearchKey)
      return c
    case "openai":
      var c = ProviderConfig.openAI
      if let m = d.string(forKey: "agent.model.openai"), !m.isEmpty { c.model = m }
      c.webSearch = d.bool(forKey: webSearchKey)
      return c
    default:
      var c = ProviderConfig.anthropic
      if let m = d.string(forKey: "agent.model"), !m.isEmpty { c.model = m }
      c.webSearch = d.bool(forKey: webSearchKey)
      return c
    }
  }

  /// The chat's Agent may search the web through the provider (Settings > API;
  /// off until the user turns it on: its queries leave for a search service).
  static let webSearchKey = "agent.webSearch"

  /// Where a connection keeps its chosen model.
  static func modelKey(_ provider: String) -> String {
    provider == "chatgpt" ? "agent.model.chatgpt" : provider == "openai" ? "agent.model.openai" : "agent.model"
  }

  /// Where a connection keeps the models listed in Settings > API.
  static func modelsKey(_ provider: String) -> String { "agent.models.\(provider)" }
}
