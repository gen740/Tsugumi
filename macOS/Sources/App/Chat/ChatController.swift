import FlatBuffers
import Foundation
import Observation
import TsugumiAgent
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
      case .display(let k): return "可視化 \(k)"
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
    await session.environments.setExtraFolders(session.identity, folders)
    await session.restartEnvironment()
  }

  /// No longer offered to the Agent (the share goes when the environment next starts).
  func detach(folder path: String) {
    folders.removeAll { $0 == path }
    Task { await session.environments.setExtraFolders(session.identity, folders) }
  }
  private(set) var available = true
  @ObservationIgnored private var agent: Agent?
  @ObservationIgnored private var catalog: ToolCatalog?
  @ObservationIgnored private var replyNames: [UInt8: String] = [:]
  @ObservationIgnored private let dir: URL

  var provider: ProviderConfig { AgentSettings.current() }

  /// The references the Agent may read (reference_read): the bundled demos,
  /// the Python SDK and the Card guide; next to the repository in a
  /// development build.
  static let references: [String: URL] = {
    let fm = FileManager.default
    if let res = Bundle.main.resourceURL, fm.fileExists(atPath: res.appendingPathComponent("reference/card-guide.md").path) {
      return ["demos": res.appendingPathComponent("demos", isDirectory: true),
              "sdk/python/tsugumi/__init__.py": res.appendingPathComponent("reference/tsugumi-sdk.py"),
              "card-guide.md": res.appendingPathComponent("reference/card-guide.md")]
    }
    let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../..").standardizedFileURL
    return ["demos": repo.appendingPathComponent("demos", isDirectory: true),
            "sdk/python/tsugumi/__init__.py": repo.appendingPathComponent("sdk/python/tsugumi/__init__.py"),
            "card-guide.md": repo.appendingPathComponent("agent/Reference/card-guide.md")]
  }()

  init(session: ProjectSession) {
    self.session = session
    thoughts = ThoughtSummarizer(presence: session.presence)
    dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Tsugumi/agent/\(session.identity.id)", isDirectory: true)
    guard let bfbs = RPCSchema.bfbs(), let schema = try? BFBS(data: bfbs), let catalog = try? ToolCatalog(schema: schema) else {
      status = "RPC スキーマを読み込めないため、チャットは使えません。通常の操作はすべて UI から行えます。"
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
      let c = Conversation.load(f)
      return SessionInfo(id: f.deletingPathExtension().lastPathComponent, title: c.title ?? firstText(c) ?? "新しい会話",
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
    Task { await agent?.cancel() }
    busy = false
    failed = false
    status = nil
    currentId = id
    let agent = Agent(catalog: catalog, transport: RPCToolTransport(session: session, replyNames: replyNames),
                      provider: provider, storage: url(id), references: Self.references)
    self.agent = agent
    items = []
    Task {
      let conv = await agent.conversation
      usage = conv.usage
      load(conv)
      // Resume work left unfinished by a disconnect or restart.
      await agent.resume { e in Task { @MainActor in self.handle(e) } }
    }
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
        case .toolResult(_, let c, let e):
          if let j = out.lastIndex(where: { $0.role == "tool" && $0.tool?.ok == nil }) {
            out[j].tool?.ok = !e
            out[j].text += "\n→ " + String(c.prefix(400))
          }
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
    refreshSessions()
    if id == currentId { select(sessions.first?.id ?? UUID().uuidString.lowercased()) }
  }

  /// Full-text search over every session, compacted parts included.
  func search(_ text: String) -> [SearchHit] {
    guard !text.isEmpty else { return [] }
    var hits: [SearchHit] = []
    for s in sessions {
      let c = Conversation.load(url(s.id))
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

  func reloadSkills() {
    var out = loadSkills(in: session.identity.url.appendingPathComponent("skills"), source: "プロジェクト")
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
    guard let agent, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
    var prompt = text
    let used = skills(in: text)
    for s in used { prompt += "\n\n[Skill \(s.name)]\n\(s.content)" }
    let refs = references + selection.filter { k in !references.contains(.card(k)) }.map { Reference.card($0) }
    if !refs.isEmpty { prompt += "\n\n[References]\n" + refs.map(describe).joined(separator: "\n") }
    if !folders.isEmpty {
      prompt += "\n\n[Attached folders]\n" + folders.map { "\($0) (read with attached_folder_read, folder: \"\($0)\")" }.joined(separator: "\n")
    }
    items.append(Item(role: "user", text: text + (refs.isEmpty ? "" : "\n📎 " + refs.map(\.label).joined(separator: ", "))))
    references = []
    begin()
    Task {
      await agent.setProvider(provider)
      await agent.send(prompt: prompt, title: text) { e in Task { @MainActor in self.handle(e) } }
    }
  }

  /// Sends a request the app wrote for the model; the chat shows `display`.
  private func sendRequest(display: String, prompt: String) {
    guard let agent else { return }
    items.append(Item(role: "user", text: display))
    begin()
    Task {
      await agent.setProvider(provider)
      await agent.send(prompt: prompt, title: display, display: display) { e in Task { @MainActor in self.handle(e) } }
    }
  }

  /// Asks the Agent to implement the Draft Cards of a scope (after they were wired and saved).
  func generateDrafts(scope: [String], cards: [CardKey]) {
    let list = cards.map { k -> String in
      // A Card's definition folder is named by its id.
      let def = path(k).last ?? "-"
      let meta = session.cardMeta(k)
      return "- \(k) (definition \(def)): \(meta?["description"]?.string ?? "")\n  content: \(meta?["content"]?.string ?? "")"
    }.joined(separator: "\n")
    let where_ = scope.isEmpty ? "the root workflow" : "the child workflow \(scope.joined(separator: "/"))"
    let names = cards.map { path($0).last ?? $0 }.joined(separator: "、")
    sendRequest(display: String(localized: "Draft \(cards.count) 枚（\(names)）から Card を生成します"), prompt: """
    Implement these Draft Cards in \(where_), matching each one's description, content, provisional ports and the \
    existing connections, and keeping the Card ids and wiring. Use a built-in Card where it is enough (data already in \
    the project is handed on by an input Card with a File output; see the rules), otherwise a code Card (language, \
    default.nix, sources and _outputs.fbs). Look at the bundled demos with reference_read when unsure how to write \
    something, and check planned changes with project_validate {"changes": [...]} instead of saving trials.
    Work one Card at a time, upstream first: save it, project_validate, run the workflow from its trigger Card, and \
    confirm the Card's result with run_record before going on to the next. At the end, report what each Card does \
    and what the run produced.
    \(list)
    """)
  }

  func cancel() {
    Task { await agent?.cancel() }
    busy = false
    session.presence.turnEnded()
    status = "回答を停止しました。受理済みの実行は止まりません（実行の停止は Canvas の「停止」）。"
  }

  /// Goes on after the step limit stopped the Agent.
  func continueWork() {
    guard let agent else { return }
    items.append(Item(role: "user", text: String(localized: "（止まったところから続けます）")))
    begin()
    Task {
      await agent.setProvider(provider)
      await agent.continueAfterLimit { e in Task { @MainActor in self.handle(e) } }
    }
  }

  func retry() {
    guard let agent else { return }
    begin()
    Task {
      await agent.setProvider(provider)
      await agent.retry { e in Task { @MainActor in self.handle(e) } }
    }
  }

  func compact() {
    guard let agent else { return }
    begin()
    status = "会話を圧縮しています…"
    Task {
      await agent.setProvider(provider)
      await agent.compact { e in Task { @MainActor in self.handle(e) } }
    }
  }

  private func begin() {
    busy = true
    failed = false
    stoppedAtLimit = false
    status = nil
    session.presence.name = { [weak session] k in session?.nodeData(k).title ?? k }
    session.presence.turnStarted()
    thoughts.turnStarted()
  }

  /// The tsugumi on the canvas follows what the Agent looks at and runs.
  private func showOnCanvas(tool: String, arguments: String) {
    switch PresenceEvents.look(tool: tool, arguments: arguments) {
    case .visit(let p)?: if session.instance(key(p)) != nil { session.presence.enqueue(.visit(key(p))) }
    case .run(let p)?: if session.instance(key(p)) != nil { session.presence.enqueue(.run(key(p))) }
    case nil: break
    }
  }

  private func handle(_ e: AgentEvent) {
    switch e {
    case .text(let t):
      thoughts.note(text: t)
      if let last = items.last, last.role == "assistant", last.tool == nil {
        items[items.count - 1].text += t
      } else {
        items.append(Item(role: "assistant", text: t))
      }
    case .toolStarted(_, let name, let args):
      items.append(Item(role: "tool", text: args, tool: (name, nil)))
      showOnCanvas(tool: name, arguments: args)
      thoughts.note(tool: name, arguments: args)
    case .toolFinished(_, let name, let ok, let summary):
      thoughts.note(tool: name, ok: ok, summary: summary)
      if let i = items.lastIndex(where: { $0.tool?.name == name && $0.tool?.ok == nil }) {
        items[i].tool = (name, ok)
        items[i].text += "\n→ " + summary
      }
    case .compacted:
      // Earlier messages were summarized for the model; the whole text stays
      // in the conversation, so nothing is announced.
      reloadCurrent()
    case .retrying(let s): status = "再試行中: \(s)"
    case .error(let s):
      busy = false
      session.presence.turnEnded()
      failed = s != "cancelled"
      status = s == "cancelled" ? "回答を停止しました（Workflow の実行は止めていません）。" : "エラー: \(s)"
      refreshAfterTurn()
    case .finished:
      busy = false
      session.presence.turnEnded()
      refreshAfterTurn()
    case .stepLimit(let steps, let run):
      busy = false
      session.presence.turnEnded()
      stoppedAtLimit = true
      status = String(localized: "\(steps) 手の上限で中断しました。「続ける」で続きから再開できます。")
        + (run.map { String(localized: " 実行 \(String($0.prefix(8))) の結果はまだ確かめていません。") } ?? "")
      refreshAfterTurn()
    }
  }

  private func reloadCurrent() {
    guard let agent else { return }
    Task { load(await agent.conversation) }
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
  static func current() -> ProviderConfig {
    let d = UserDefaults.standard
    if d.string(forKey: "agent.provider") == "fixture" {
      return .localFixture(port: d.integer(forKey: "agent.fixturePort") == 0 ? 8765 : d.integer(forKey: "agent.fixturePort"))
    }
    if d.string(forKey: "agent.provider") == "openai" {
      var c = ProviderConfig.openAI
      if let m = d.string(forKey: "agent.model.openai"), !m.isEmpty { c.model = m }
      return c
    }
    var c = ProviderConfig.anthropic
    if let m = d.string(forKey: "agent.model"), !m.isEmpty { c.model = m }
    return c
  }
}
