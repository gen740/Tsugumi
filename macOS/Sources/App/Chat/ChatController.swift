import CryptoKit
import FlatBuffers
import Foundation
import Observation
import SwiftUI
import TsugumiAgentClient
import TsugumiLauncher
import TsugumiRPC

/// Registers the selected project endpoint with core's Agent service.
struct RPCToolTransport: ToolTransport {
  let session: ProjectSession
  @MainActor func project() async throws -> AgentProject? {
    let endpoint: Endpoint
    if let override = session.endpointOverride { endpoint = override }
    else { endpoint = try await session.environments.start(session.identity) }
    return AgentProject(id: endpoint.projectId, endpoint: endpoint.url.absoluteString, token: endpoint.token,
                        title: session.identity.url.lastPathComponent, path: session.identity.url.standardizedFileURL.path)
  }
}

/// A skill: instructions attached to a message with `$name`.
struct Skill: Identifiable, Hashable {
  var id: String { path }
  let name: String
  let description: String
  let path: String
  let source: String
  let sourceFolder: String
  let attachmentFolder: String
}

/// Adapts core-owned metadata for the folder list and completion menu.
@MainActor
func availableSkills(project: URL? = nil, folders: [String], conversationID: String? = nil) async throws -> (skills: [Skill], missing: [String]) {
  var fields: [String: Any] = ["folders": folders]
  if let conversationID { fields["conversation_id"] = conversationID }
  if let project { fields["project_path"] = project.standardizedFileURL.path }
  let value = try await CoreAgentClient.shared.call("SkillsListRequest", fields)
  let skills = (value["skills"] as? [[String: String]] ?? []).filter { $0["status"] == "ready" && (project != nil || $0["path"] != "builtin:demo") }.map { s in
    let path = s["path"] ?? ""
    if path == "builtin:demo" { return ChatController.demoSkill }
    let folder = s["source_folder"] ?? ""
    let source = folder == project?.appendingPathComponent("skills").path ? String(localized: "プロジェクト") : URL(fileURLWithPath: folder).lastPathComponent
    return Skill(name: s["name"] ?? "", description: s["description"] ?? "", path: path, source: source, sourceFolder: folder, attachmentFolder: s["attachment_folder"] ?? "")
  }
  let missing = (value["folders"] as? [[String: String]] ?? []).filter { $0["status"] != "ready" }.compactMap { $0["path"] }
  return (skills, missing)
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
  /// "Draw thoughts": the Agent's thinking in one line for the canvas.
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
  private var folderResources: [String: String] = [:]
  private(set) var attaching = false

  /// Share a batch with one restart, then register each folder independently.
  /// Only registered folders are offered to the Agent; a failed attempt is retryable.
  func attach(folders urls: [URL]) async {
    guard !attaching else { return }
    var paths: [String] = []
    for url in urls {
      let path = url.standardizedFileURL.path
      if folderResources[path] == nil && !paths.contains(path) { paths.append(path) }
    }
    guard !paths.isEmpty else { return }
    attaching = true
    defer { attaching = false }
    do {
      for path in paths {
        var directory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: path, isDirectory: &directory), directory.boolValue else {
          throw CocoaError(.fileReadNoSuchFile, userInfo: [NSFilePathErrorKey: path])
        }
      }
      let shared = folders + paths.filter { !folders.contains($0) }
      try await session.environments.setExtraFolders(session.identity, shared)
      guard session.connected, let project = try await RPCToolTransport(session: session).project() else {
        throw RPCError.disconnected
      }
      for path in paths {
        let resource = "folder:\(project.id):" + SHA256.hash(data: Data(path.utf8)).map { String(format: "%02x", $0) }.joined()
        _ = try await CoreAgentClient.shared.call("RegisterFolderRequest", ["project_id": project.id,
          "resource_id": resource, "folder": path, "title": (path as NSString).lastPathComponent])
        folderResources[path] = resource
        if !folders.contains(path) { folders.append(path) }
      }
      status = nil
    } catch { status = String(describing: error) }
  }

  /// No longer offered to the Agent, which refuses it from now on (the share
  /// itself goes when the environment next starts).
  func detach(folder path: String) {
    guard !attaching else { return }
    folders.removeAll { $0 == path }
    let resource = folderResources.removeValue(forKey: path)
    let folders = self.folders
    Task {
      do {
        if let resource {
          let access = try await CoreAgentClient.shared.call("AccessListRequest")
          for grant in access["grants"] as? [[String: Any]] ?? [] where grant["project_id"] as? String == resource && grant["active"] as? Bool == true {
            if let id = grant["id"] as? String { _ = try await CoreAgentClient.shared.call("AccessRevokeRequest", ["grant_id": id]) }
          }
        }
        try await session.environments.setExtraFolders(session.identity, folders)
      } catch { status = String(describing: error) }
    }
  }
  private(set) var interrupted = false
  private(set) var interruptedOperation: Tsugumi_Agent_InterruptedOperationT?
  private(set) var available = true
  @ObservationIgnored private var agent: Agent?
  /// Reads a snapshot and attaches to work that is already running.
  @ObservationIgnored private var opening: Task<Void, Never>?
  @ObservationIgnored private var stopping: Task<Void, Never>?
  @ObservationIgnored private let dir: URL

  @ObservationIgnored private let connection: @MainActor () -> ProviderConfig
  var provider: ProviderConfig { connection() }

  /// `directory` and `connection`: where its conversations are and what it
  /// answers with, instead of the app's (tests).
  init(session: ProjectSession, directory: URL? = nil, connection: @escaping @MainActor () -> ProviderConfig = AgentSettings.chat) {
    self.session = session
    self.connection = connection
    thoughts = ThoughtSummarizer(presence: session.presence)
    dir = directory ?? AppChannel.current.supportDirectory.appendingPathComponent("agent/\(session.identity.id)", isDirectory: true)
    refreshSessions()
    reloadSkills()
  }

  // MARK: - Sessions

  private func url(_ id: String) -> URL { dir.appendingPathComponent("sessions/\(id).json") }

  /// Conversations are listed from core and are independent of UI sessions.
  func refreshSessions() {
    Task {
      do {
        let value = try await CoreAgentClient.shared.call("ConversationListRequest")
        sessions = (value["value"] as? [[String: Any]] ?? []).compactMap { item in
          guard let id = item["id"] as? String else { return nil }
          return SessionInfo(id: id, title: item["title"] as? String ?? String(localized: "新しい会話"),
            updatedAt: Date(timeIntervalSince1970: (item["updatedAt"] as? Double ?? 0) / 1000), messages: item["messages"] as? Int ?? 0)
        }
        if currentId.isEmpty { select(sessions.first?.id ?? UUID().uuidString.lowercased()) }
      } catch { status = String(describing: error) }
    }
  }

  func select(_ id: String) {
    // Observation moves to this conversation; the previous one stays in core.
    let previous = agent
    if let previous { Task { await previous.detach() } }
    opening?.cancel()
    opening = nil
    answer += 1
    busy = false
    failed = false
    usageLimited = false
    status = nil
    interrupted = false
    interruptedOperation = nil
    stoppedAtLimit = false
    demoMode = false
    currentId = id
    thoughts.conversationID = id
    let agent = Agent(id: id, transport: RPCToolTransport(session: session), provider: provider)
    self.agent = agent
    items = []
    // (the approver holds the controller weakly: the Agent outlives no window)
    let approver: Agent.Approver = { [weak self] asked in await self?.approve(asked) ?? [] }
    opening = Task {
      await previous?.detach()
      await agent.setApprover(approver)
      do { try await agent.open() } catch { self.status = String(describing: error); return }
      let conv = await agent.conversation
      guard !Task.isCancelled else { return }
      usage = conv.usage
      await adoptInterruption(agent)
      guard !Task.isCancelled, self.agent === agent else { return }
      load(conv)
      if await agent.hasUnfinishedWork {
        guard !Task.isCancelled, self.agent === agent else { return }
        let events = begin()
        await agent.observe(events: events)
      }
    }
  }

  private func adoptInterruption(_ agent: Agent) async {
    let interrupted = await agent.resumable
    let operation = await agent.interruptedOperation
    let continuable = await agent.canContinue
    guard self.agent === agent else { return }
    let wasInterrupted = self.interrupted
    self.interrupted = interrupted
    interruptedOperation = operation
    stoppedAtLimit = !interrupted && continuable
    if interrupted {
      failed = false
      usageLimited = false
      contextFull = false
      let doing: String
      switch operation?.kind {
      case "tool": doing = "\(operation?.name ?? "") (\(operation?.id ?? "")) \(operation?.argumentsJson ?? "")"
      case "compact": doing = String(localized: "会話の圧縮")
      case "preparing": doing = String(localized: "メッセージの準備")
      default: doing = String(localized: "モデルの応答")
      }
      status = String(format: String(localized: "会話は中断しています。中断時の処理: %@。再開するか、新しいメッセージを送れます。"), doing)
    } else if wasInterrupted { status = nil }
  }

  /// Only the Resume control calls this method.
  func resume() {
    guard interrupted, !busy, let agent else { return }
    let events = begin()
    Task {
      await agent.setProvider(provider)
      await agent.resume(events: events)
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
    for record in conv.tools where record.name == "operation_approval" {
      out.append(Item(role: "tool", text: record.result ?? "", tool: (record.name, record.state == .pending ? nil : !record.isError)))
    }
    items = out
  }

  func newSession() {
    select(UUID().uuidString.lowercased())
    refreshSessions()
  }

  func delete(_ id: String) {
    Task {
      do {
        _ = try await CoreAgentClient.shared.call("ConversationDeleteRequest", ["conversation_id": id])
        if id == currentId { currentId = "" }
        refreshSessions()
      } catch { status = String(describing: error) }
    }
  }

  func search(_ text: String) async -> [SearchHit] {
    guard !text.isEmpty else { return [] }
    guard let result = try? await CoreAgentClient.shared.call("ConversationSearchRequest", ["text": text]) else { return [] }
    return (result["value"] as? [[String: Any]] ?? []).compactMap { item in
      guard let session = item["session"] as? String else { return nil }
      return SearchHit(session: session, title: item["title"] as? String ?? "", snippet: item["snippet"] as? String ?? "")
    }
  }

  // MARK: - Skills and references

  /// The built-in $demo: Tsugumi of this chat shows the screen around (it flies only then).
  static let demoSkill = Skill(name: "demo", description: String(localized: "ツグミが画面を飛び回って、デモや画面の使い方を案内します"),
                               path: "builtin:demo", source: "Tsugumi", sourceFolder: "", attachmentFolder: "")

  func reloadSkills() {
    Task {
      do { skills = try await loadSkillMetadata() }
      catch { status = String(describing: error) }
    }
  }

  private func loadSkillMetadata(conversationID: String? = nil) async throws -> [Skill] {
    try await availableSkills(project: session.identity.url,
      folders: AppPreferences.shared.skillFolders.filter(\.enabled).map(\.path), conversationID: conversationID).skills
  }

  private func attachSkillFolders(_ selected: [Skill]) async {
    for folder in Set(selected.map(\.attachmentFolder)) where !folder.isEmpty {
      if folderResources[folder] == nil { await attach(folders: [URL(fileURLWithPath: folder)]) }
    }
  }

  func addReference(_ r: Reference) {
    if !references.contains(r) { references.append(r) }
  }

  /// `$name` tokens that match a skill.
  func skills(in text: String) -> [Skill] {
    let names = Set(text.split(whereSeparator: { $0.isWhitespace }).filter { $0.hasPrefix("$") }.map { String($0.dropFirst()) })
    return skills.filter { names.contains($0.name) }
  }

  // MARK: - Conversation

  func send(_ text: String, selection: [CardKey] = []) {
    guard !attaching, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
    // No model connection: Tsugumi answers from its script (a connection of this chat's own, as in tests, is used).
    if GuideController.shared.scripted, provider.auth != .none, !busy {
      GuideController.shared.answerScripted(text, chat: self)
      return
    }
    guard let agent else { return }
    let used = skills(in: text)
    if used.contains(where: { $0.name == "demo" }) { demoMode = true }
    let refs = references + selection.filter { k in !references.contains(.card(k)) }.map { Reference.card($0) }
    let shown = text + (refs.isEmpty ? "" : "\n📎 " + refs.map(\.label).joined(separator: ", "))
    items.append(Item(role: "user", text: shown))
    references = []
    let events = begin()
    let stopping = self.stopping
    Task {
      await stopping?.value
      do {
        skills = try await loadSkillMetadata(conversationID: agent.id)
        await attachSkillFolders(skills.filter { Set(used.map(\.name)).contains($0.name) })
        await CorePreferences.shared.reload()
        await agent.setProvider(provider)
        // Only resource identifiers leave the UI; core reads after authorization.
        await agent.send(prompt: text, title: text, display: shown,
                       selectedCards: refs.compactMap { if case .card(let key) = $0 { return key }; return nil },
                       selectedDisplays: refs.compactMap { if case .display(let key) = $0 { return key }; return nil },
                       skillNames: Array(Set(used.map(\.name))),
                       attachedResources: folders.compactMap { folderResources[$0] }, events: events)
      } catch { events(.error(String(describing: error))) }
    }
  }

  /// Demo mode ($demo, or a demo tour): the bird leads the user about the
  /// screen for this chat; it ends with the tour's farewell. Otherwise the chat
  /// is a chat, whose bird flies only to show where a feature asked about is.
  private(set) var demoMode = false

  /// Replies to tap and buttons Tsugumi offered with its last answer (guide_show).
  private(set) var choices: [String] = []
  private(set) var offered: [GuideAction] = []
  /// Without a model connection: Tsugumi's fixed reply from agentd to what
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
    demoMode = true
    if busy {
      notes.append((display, display))
      return
    }
    sendRequest(display: display, prompt: display)
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
    let stopping = self.stopping
    Task {
      await stopping?.value
      await agent.setProvider(provider)
      await agent.send(prompt: prompt, title: display.components(separatedBy: "\n")[0], display: display, skillNames: demoMode ? ["demo"] : [], events: events)
    }
  }

  /// Asks the Agent to implement the Draft Cards of a scope (after they were wired and saved).
  func generateDrafts(scope: [String], cards: [CardKey]) {
    sendRequest(display: String(localized: "Draft \(cards.count) 枚から Card を生成します"),
      prompt: "Implement the selected Draft Cards in scope \(scope): \(cards). Read their definitions after access is granted.")
  }

  func generateFromComments(scope: [String], cards: [CardKey]) {
    sendRequest(display: String(localized: "選択した Card のコメントに従って生成します"),
      prompt: "Resolve the open comments on these Cards in scope \(scope): \(cards). Read their comments after access is granted.")
  }

  // MARK: - Card permissions

  /// A save of the Agent waiting for the user: what it would give Cards.
  struct PermissionPrompt: Identifiable {
    let id = UUID()
    let raw: [String]
    var requests: [CardPermission.Request] { raw.compactMap(CardPermission.Request.init) }
    var operation: String? {
      guard let request = raw.first(where: { $0.contains("operation_approval\n") }),
            let marker = request.range(of: "operation_approval\n") else { return nil }
      return request[marker.upperBound...].components(separatedBy: "\n").map { line in
        guard let data = line.data(using: .utf8),
              let value = try? JSONSerialization.jsonObject(with: data),
              let formatted = try? JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys]),
              let text = String(data: formatted, encoding: .utf8) else { return line }
        return text
      }.joined(separator: "\n")
    }
    var access: [String] { raw.filter { $0.hasPrefix("project_access ") } }
    let answer: (Set<String>) -> Void
  }
  var permissionPrompt: PermissionPrompt?

  /// The widenings the user approves: those of the kinds allowed without
  /// asking at once, the others when asked (nothing when declined).
  func approve(_ asked: [String]) async -> Set<String> {
    permissionPrompt?.answer([])
    return await withCheckedContinuation { continuation in
      permissionPrompt = PermissionPrompt(raw: asked) { continuation.resume(returning: $0) }
    }
  }

  /// The user's answer; `always` are kinds allowed without asking from now on.
  func answerPermissions(allow: Bool, always: Set<CardPermission>) {
    guard let p = permissionPrompt else { return }
    permissionPrompt = nil
    if allow && p.operation == nil { AppPreferences.shared.allowedPermissions.formUnion(always.map(\.rawValue)) }
    p.answer(allow ? Set(p.raw) : [])
  }

  /// A window releases its UI subscriptions without cancelling core work.
  func detach() {
    opening?.cancel()
    opening = nil
    answer += 1
    let remote = agent
    agent = nil
    Task {
      await GuideController.shared.detach(from: session)
      await remote?.detach()
      // Both observers are cancelled before releasing a pending UI answer.
      // Their cancellation check prevents this from denying the core request.
      let prompt = permissionPrompt
      permissionPrompt = nil
      prompt?.answer([])
    }
  }

  func cancel() {
    if let p = permissionPrompt {
      permissionPrompt = nil
      p.answer([])
    }
    opening?.cancel()
    opening = nil
    answer += 1
    let remote = agent
    stopping = Task { await remote?.cancel() }
    busy = false
    stoppedAtLimit = true
    session.presence.turnEnded(.stopped)
    status = String(localized: "回答を停止しました。受理済みの実行は止まりません（実行の停止は Canvas の「停止」）。")
  }

  /// Goes on after the step limit stopped the Agent.
  func continueWork() {
    guard let agent else { return }
    items.append(Item(role: "user", text: String(localized: "（止まったところから続けます）")))
    let events = begin()
    let stopping = self.stopping
    Task {
      await stopping?.value
      await agent.setProvider(provider)
      await agent.continueAfterLimit(events: events)
    }
  }

  func retry() {
    guard let agent else { return }
    let events = begin()
    let stopping = self.stopping
    Task {
      await stopping?.value
      await agent.setProvider(provider)
      await agent.retry(events: events)
    }
  }

  func compact() {
    guard let agent else { return }
    let events = begin()
    status = String(localized: "会話を圧縮しています…")
    let stopping = self.stopping
    Task {
      await stopping?.value
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
    interrupted = false
    interruptedOperation = nil
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
      bird.doing(PresenceEvents.activity(tool: tool))
      showOnCanvas(tool: tool, arguments: args, bird: bird)
      if let row { items[row].tool = (tool, nil); items[row].text = args }
    case .toolFinished(_, let tool, let ok, _):
      if PresenceEvents.writesSources(tool: tool) { bird.toolInputEnded(ok: ok) }
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
    case .toolFinished(_, let name, let ok, let summary):
      if PresenceEvents.writesSources(tool: name) { session.presence.toolInputEnded(ok: ok) }
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
      if failed || stoppedAtLimit {
        refreshAfterTurn()
        return
      }
      GuideController.shared.chatTurnEnded(self, reply: lastReply)
      busy = false
      // A note of this turn (a retry, the compaction) is no longer current.
      status = nil
      session.presence.turnEnded()
      refreshAfterTurn()
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
    let number = answer
    Task {
      let conversation = await agent.conversation
      guard number == answer else { return }
      load(conversation)
      usage = conversation.usage
    }
  }

  private func refreshAfterTurn() {
    guard let agent else { return }
    let number = answer
    Task {
      let conversation = await agent.conversation
      guard number == answer else { return }
      await adoptInterruption(agent)
      load(conversation)
      usage = conversation.usage
      refreshSessions()
    }
  }
}

@MainActor
enum AgentSettings {
  /// The small model Tsugumi speaks with on the ChatGPT plan (the Home guide,
  /// the canvas bird's thoughts): gpt-6.1-luna when the account offers it,
  /// else its newest luna ("agent.speechModel.chatgpt" overrides it).
  static var chatGPTSpeechModel: String {
    let d = CorePreferences.shared
    if let m = d.string(forKey: "agent.speechModel.chatgpt"), !m.isEmpty { return m }
    return speechModel(offered: ModelCatalogSnapshot.shared.models("chatgpt").map(\.id), fallback: config(for: "chatgpt").model)
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
  static func guide() async -> ProviderConfig {
    await CorePreferences.shared.reload()
    _ = try? await CoreAgentClient.shared.storedModels("chatgpt")
    var c = current()
    if c.auth == .chatGPT { c.model = chatGPTSpeechModel }
    else if c.auth == .apiKey, c.api == .anthropicMessages { c.model = "claude-haiku-5-5" }
    return c
  }

  /// The connection chosen in Settings > API ("anthropic", "openai", "chatgpt" or "fixture").
  static var provider: String { CorePreferences.shared.string(forKey: "agent.provider") ?? "anthropic" }

  static func current() -> ProviderConfig { config(for: provider) }

  /// The connection of the chat's Agent and its helpers: with the effort and
  /// the Fast mode chosen in the model menu. The guide, the summaries and the
  /// checks use `current()`, without them.
  static func chat() -> ProviderConfig {
    let d = CorePreferences.shared
    return current().choosing(effort: d.string(forKey: effortKey) ?? "", fastModel: d.string(forKey: fastModelKey) ?? "")
  }

  /// The model menu's effort ("" or none: the model's default), for every model that takes it.
  static let effortKey = "agent.effort"
  /// The one model Fast mode was turned on for.
  static let fastModelKey = "agent.fastModel"

  /// A connection with its chosen model (each keeps its own).
  static func config(for provider: String) -> ProviderConfig {
    let d = CorePreferences.shared
    switch provider {
    case "fixture":
      return .fixture
    case "chatgpt":
      var c = ProviderConfig.chatGPT
      if let m = d.string(forKey: "agent.model.chatgpt"), !m.isEmpty { c.model = m }
      return c
    case "openai":
      var c = ProviderConfig.openAI
      if let m = d.string(forKey: "agent.model.openai"), !m.isEmpty { c.model = m }
      return c
    default:
      var c = ProviderConfig.anthropic
      if let m = d.string(forKey: "agent.model"), !m.isEmpty { c.model = m }
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

}
