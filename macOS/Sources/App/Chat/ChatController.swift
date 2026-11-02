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
  private(set) var items: [Item] = []
  private(set) var busy = false
  private(set) var status: String?
  private(set) var failed = false
  private(set) var sessions: [SessionInfo] = []
  private(set) var currentId: String = ""
  private(set) var usage = Usage()
  private(set) var skills: [Skill] = []
  var references: [Reference] = []
  private(set) var available = true
  @ObservationIgnored private var agent: Agent?
  @ObservationIgnored private var catalog: ToolCatalog?
  @ObservationIgnored private var replyNames: [UInt8: String] = [:]
  @ObservationIgnored private let dir: URL

  var provider: ProviderConfig { AgentSettings.current() }

  init(session: ProjectSession) {
    self.session = session
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
    migrateLegacy()
    refreshSessions()
    select(sessions.first?.id ?? UUID().uuidString.lowercased())
    reloadSkills()
  }

  // MARK: - Sessions

  private func url(_ id: String) -> URL { dir.appendingPathComponent("sessions/\(id).json") }

  /// The single conversation.json of earlier versions becomes the first session.
  private func migrateLegacy() {
    let old = dir.appendingPathComponent("conversation.json")
    guard FileManager.default.fileExists(atPath: old.path) else { return }
    let c = Conversation.load(old)
    try? c.save(url(c.id))
    try? FileManager.default.removeItem(at: old)
  }

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
                      provider: provider, storage: url(id))
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
    if conv.summary != nil {
      let at = out.firstIndex { !$0.archived } ?? out.count
      out.insert(Item(role: "note", text: "ここまでの会話は要約してモデルに渡しています（全文は残っています）。"), at: at)
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
      var s = "Card \(k) (\(session.kind(k).label), definition \(session.instance(k)?["definition"]?.string ?? "-"), name \"\(session.displayName(k))\""
      if let st = session.cardStates[k] { s += ", last state \(st.state.label)" }
      if let r = session.resultSummary(k) { s += ", result: \(r)" }
      return s + ")"
    case .display(let k):
      switch session.displays[k]?.value {
      case .text(let t)?: return "Display of Card \(k) (text):\n\(t.prefix(4000))"
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
    items.append(Item(role: "user", text: text + (refs.isEmpty ? "" : "\n📎 " + refs.map(\.label).joined(separator: ", "))))
    references = []
    begin()
    Task {
      await agent.setProvider(provider)
      await agent.send(prompt: prompt, title: text) { e in Task { @MainActor in self.handle(e) } }
    }
  }

  /// Asks the Agent to implement the Draft Cards of a scope (after they were wired and saved).
  func generateDrafts(scope: [String], cards: [CardKey]) {
    let list = cards.map { k -> String in
      let def = session.instance(k)?["definition"]?.string ?? "-"
      let meta = session.definitionMeta[def]
      return "- \(k) (definition \(def)): \(meta?["description"]?.string ?? "")\n  plan: \(meta?["plan"]?.string ?? "")"
    }.joined(separator: "\n")
    let where_ = scope.isEmpty ? "the root workflow" : "the child workflow \(scope.joined(separator: "/"))"
    send("""
    Implement these Draft Cards in \(where_). For each one, turn its definition into a working Card (language, \
    default.nix, sources and _outputs.fbs) that matches its description, plan, provisional ports and the existing \
    connections, keeping the instance ids and wiring. Save with project_save, then validate and report what you changed.
    \(list)
    """)
  }

  func cancel() {
    Task { await agent?.cancel() }
    busy = false
    status = "回答を停止しました。受理済みの実行は止まりません（実行の停止は Canvas の「停止」）。"
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
    status = nil
  }

  private func handle(_ e: AgentEvent) {
    switch e {
    case .text(let t):
      if let last = items.last, last.role == "assistant", last.tool == nil {
        items[items.count - 1].text += t
      } else {
        items.append(Item(role: "assistant", text: t))
      }
    case .toolStarted(_, let name, let args):
      items.append(Item(role: "tool", text: args, tool: (name, nil)))
    case .toolFinished(_, let name, let ok, let summary):
      if let i = items.lastIndex(where: { $0.tool?.name == name && $0.tool?.ok == nil }) {
        items[i].tool = (name, ok)
        items[i].text += "\n→ " + summary
      }
    case .compacted:
      status = "古いメッセージを要約しました（全文の履歴は残っています）。"
      reloadCurrent()
    case .retrying(let s): status = "再試行中: \(s)"
    case .error(let s):
      busy = false
      failed = s != "cancelled"
      status = s == "cancelled" ? "回答を停止しました（Workflow の実行は止めていません）。" : "エラー: \(s)"
      refreshAfterTurn()
    case .finished:
      busy = false
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
    var c = ProviderConfig.anthropic
    if let m = d.string(forKey: "agent.model"), !m.isEmpty { c.model = m }
    return c
  }
}
