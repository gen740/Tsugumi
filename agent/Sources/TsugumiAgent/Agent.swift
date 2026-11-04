import FlatBuffers
import Foundation

/// Sends one core request and returns the raw reply. Implemented by the app
/// with its authenticated RPC client; the Agent never touches UI state.
public protocol ToolTransport: Sendable {
  /// `build` writes the request table into the builder; the transport wraps it
  /// in Request/Message. Returns the reply union type name and its table.
  func call(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset) async throws -> (replyType: String, table: Table)
}

/// Durable trace of one tool call.
public struct ToolRecord: Codable, Equatable {
  public enum State: String, Codable { case pending, sent, completed, skipped }
  public var id: String
  public var name: String
  public var arguments: String
  public var opId: String?
  public var state: State
  public var result: String?
  public var isError = false

  public init(id: String, name: String, arguments: String, opId: String? = nil, state: State,
              result: String? = nil, isError: Bool = false) {
    self.id = id
    self.name = name
    self.arguments = arguments
    self.opId = opId
    self.state = state
    self.result = result
    self.isError = isError
  }
}

/// Conversation persisted per project on the host (messages, tool trace, usage).
public struct Conversation: Codable, Equatable {
  public var id = UUID().uuidString.lowercased()
  public var title: String?
  public var updatedAt = Date()
  public var messages: [ChatMessage] = []
  /// Messages replaced by the summary when the context was compacted. The
  /// model no longer sees them, but the full history stays readable and searchable.
  public var archive: [ChatMessage] = []
  public var tools: [ToolRecord] = []
  public var usage = Usage()
  public var summary: String?  // compacted history

  public init() {}

  enum CodingKeys: String, CodingKey { case id, title, updatedAt, messages, archive, tools, usage, summary }

  public init(from d: Decoder) throws {
    let c = try d.container(keyedBy: CodingKeys.self)
    id = try c.decode(String.self, forKey: .id)
    title = try c.decodeIfPresent(String.self, forKey: .title)
    updatedAt = try c.decodeIfPresent(Date.self, forKey: .updatedAt) ?? Date(timeIntervalSince1970: 0)
    messages = try c.decode([ChatMessage].self, forKey: .messages)
    archive = try c.decodeIfPresent([ChatMessage].self, forKey: .archive) ?? []
    tools = try c.decode([ToolRecord].self, forKey: .tools)
    usage = try c.decode(Usage.self, forKey: .usage)
    summary = try c.decodeIfPresent(String.self, forKey: .summary)
  }

  /// Everything said in this conversation, including compacted messages.
  public var fullHistory: [ChatMessage] { archive + messages }

  /// A saved conversation; throws when the file is missing or unreadable.
  public static func read(_ url: URL) throws -> Conversation {
    try JSONDecoder().decode(Conversation.self, from: Data(contentsOf: url))
  }

  /// A saved conversation, or a new one when there is none (or it is unreadable).
  public static func load(_ url: URL) -> Conversation { (try? read(url)) ?? Conversation() }

  public func save(_ url: URL) throws {
    try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    let enc = JSONEncoder()
    enc.outputFormatting = [.prettyPrinted, .sortedKeys]
    try enc.encode(self).write(to: url, options: .atomic)
  }
}

public enum AgentEvent: Sendable {
  case text(String)
  case toolStarted(id: String, name: String, arguments: String)
  case toolFinished(id: String, name: String, ok: Bool, summary: String)
  case compacted
  case retrying(String)
  /// The text streamed so far in this reply is dropped: the reply is asked
  /// again (a connection or provider error in the middle of it).
  case discardText
  case error(String)
  case finished
  /// The turn used its steps (model replies with tool calls) without
  /// finishing; `unconfirmedRun` is a run it started and never saw end.
  case stepLimit(steps: Int, unconfirmedRun: String?)
}

/// The Agent loop: streaming model turns, tool argument validation, real RPC
/// execution through ToolTransport, persistence and cancellation.
public actor Agent {
  public nonisolated let catalog: ToolCatalog
  let codec: FlatJSON
  let transport: ToolTransport
  let storage: URL
  public private(set) var conversation: Conversation
  private var provider: MessagesProvider
  private var turn: Task<Void, Never>?
  /// Model replies with tool calls in one turn before it stops and asks to
  /// continue (Draft generation takes many: write, check, save, run, read).
  public var maxSteps = 60
  /// Bundled references for reference_read: a top-level name ("demos",
  /// "sdk/python/tsugumi/__init__.py", "card-guide.md") -> where it is.
  private let references: [String: URL]
  /// A run started by a tool call whose end was not seen yet.
  private var unconfirmedRun: String?
  /// Replies cut at the output limit in this turn.
  private var cutReplies = 0
  /// How long run_status waits for a run to end before it answers.
  public var statusWait: TimeInterval = 300

  /// The rules and working examples the model gets. The default.nix and
  /// _outputs.fbs examples are the bundled demos' own files (a test keeps
  /// them equal), so they never drift from what works.
  public static let systemPrompt = Shared.systemPrompt

  public init(catalog: ToolCatalog, transport: ToolTransport, provider: ProviderConfig, storage: URL,
              references: [String: URL] = [:]) {
    self.references = references
    self.catalog = catalog
    self.codec = FlatJSON(schema: catalog.schema)
    self.transport = transport
    self.provider = MessagesProvider(config: provider)
    self.storage = storage
    self.conversation = Conversation.load(storage)
  }

  public func setProvider(_ config: ProviderConfig) { provider = MessagesProvider(config: config) }
  public func setMaxSteps(_ n: Int) { maxSteps = max(1, n) }
  public func models() async throws -> [String] { try await provider.models() }

  public func reset() {
    turn?.cancel()
    conversation = Conversation()
    try? conversation.save(storage)
  }

  /// Stops model streaming and tools not yet sent. Runs already accepted by the
  /// core keep running; cancelling them needs an explicit run_cancel.
  public func cancel() {
    turn?.cancel()
  }

  /// Cancels and returns once the turn has ended (nothing of it changes the
  /// conversation afterwards).
  public func cancelAndWait() async {
    turn?.cancel()
    await turn?.value
  }

  /// Starts a turn once the previous one has ended: a cancelled turn's last
  /// steps never interleave with the next one's.
  private func startTurn(_ body: @escaping @Sendable () async -> Void) {
    let previous = turn
    previous?.cancel()
    turn = Task {
      await previous?.value
      await body()
    }
  }

  public func send(_ text: String, context: [String], events: @escaping @Sendable (AgentEvent) -> Void) {
    turn?.cancel()
    var prompt = text
    if !context.isEmpty { prompt += "\n\n[Context: selected Cards \(context.joined(separator: ", "))]" }
    send(prompt: prompt, title: text, events: events)
  }

  /// Sends a prepared prompt (references and skills already attached).
  public func send(prompt: String, title: String, display: String? = nil, events: @escaping @Sendable (AgentEvent) -> Void) {
    turn?.cancel()
    if conversation.title == nil { conversation.title = String(title.prefix(60)) }
    conversation.messages.append(ChatMessage(role: "user", content: [.text(prompt)], display: display))
    conversation.updatedAt = Date()
    try? conversation.save(storage)
    startTurn { await self.loop(events) }
  }

  /// Retries after a failed or stopped answer. A turn whose model reply never
  /// arrived is continued; otherwise the last request is asked again. Tool
  /// calls already confirmed by the core are reconciled, never re-sent.
  public func retry(events: @escaping @Sendable (AgentEvent) -> Void) {
    turn?.cancel()
    if let last = conversation.messages.last, last.role == "user" {
      startTurn { await self.loop(events) }
      return
    }
    let lastText = conversation.messages.last { m in
      m.role == "user" && m.content.contains { if case .text = $0 { return true }; return false }
    }
    guard let lastText else { return }
    conversation.messages.append(lastText)
    conversation.updatedAt = Date()
    try? conversation.save(storage)
    startTurn { await self.loop(events) }
  }

  /// Goes on after the step limit stopped a turn.
  public func continueAfterLimit(events: @escaping @Sendable (AgentEvent) -> Void) {
    turn?.cancel()
    var note = "Continue from where you stopped (the step limit ended the previous turn). Check the current state first"
    if let run = unconfirmedRun { note += " and the outcome of run \(run) (run_status), which was started and never checked" }
    note += ", then finish the remaining work."
    conversation.messages.append(ChatMessage(role: "user", content: [.text(note)], display: "（止まったところから続けます）"))
    conversation.updatedAt = Date()
    try? conversation.save(storage)
    startTurn { await self.loop(events) }
  }

  /// Compacts the context now (the full history stays in the archive).
  public func compact(events: @escaping @Sendable (AgentEvent) -> Void) {
    startTurn {
      do {
        try await self.compactIfNeeded(events, force: true)
        events(.finished)
      } catch {
        events(.error("\(error)"))
      }
    }
  }

  /// Resumes after a restart or disconnect: tool calls that were sent but never
  /// answered are reconciled with OperationGet instead of being re-sent.
  public func resume(events: @escaping @Sendable (AgentEvent) -> Void) {
    guard conversation.tools.contains(where: { $0.state == .sent || $0.state == .pending }) ||
          conversation.messages.last?.role == "user" else { return }
    startTurn { await self.loop(events) }
  }

  private func loop(_ events: @escaping @Sendable (AgentEvent) -> Void) async {
    do {
      try await reconcilePending(events)
      closeDanglingToolUses()
      cutReplies = 0
      var done = false
      for _ in 0..<maxSteps {
        try Task.checkCancellation()
        try await compactIfNeeded(events)
        var (blocks, stop) = try await modelTurn(events)
        if stop == "max_tokens" {
          // The reply was cut at the output limit, maybe inside a tool call:
          // nothing of it is executed; the model is asked for smaller steps.
          cutReplies += 1
          guard cutReplies <= 3 else { throw AgentError.provider("the model's replies were cut at the output limit three times") }
          blocks = blocks.filter { if case .toolUse = $0 { return false }; return true }
          if blocks.isEmpty { blocks = [.text("[cut at the output limit]")] }
          conversation.messages.append(ChatMessage(role: "assistant", content: blocks))
          conversation.messages.append(ChatMessage(role: "user", content: [.text(
            "Your last reply was cut at the output limit, so its tool call was not run and nothing was saved. Make the "
            + "change in smaller steps: one Card per project_save, and long source files in saves of their own.")],
            display: "（応答が長すぎて途中で切れたため、小さく分けてやり直します）"))
          events(.retrying("the reply was cut at the output limit; asking for smaller steps"))
          try? conversation.save(storage)
          continue
        }
        conversation.messages.append(ChatMessage(role: "assistant", content: blocks))
        try? conversation.save(storage)
        let uses = blocks.compactMap { b -> (String, String, String)? in
          if case .toolUse(let id, let name, let input) = b { return (id, name, input) }
          return nil
        }
        if uses.isEmpty || stop != "tool_use" {
          done = true
          break
        }
        var results: [ContentBlock] = []
        for (id, name, input) in uses {
          try Task.checkCancellation()
          results.append(await runTool(id: id, name: name, input: input, events))
        }
        // Cancelled meanwhile: the results are in the tool records; the next
        // turn answers the tool calls from them (never this cancelled one).
        try Task.checkCancellation()
        conversation.messages.append(ChatMessage(role: "user", content: results))
        try? conversation.save(storage)
      }
      // Never a silent stop: the limit is its own outcome, and it can go on.
      events(done ? .finished : .stepLimit(steps: maxSteps, unconfirmedRun: unconfirmedRun))
    } catch where error is CancellationError || Task.isCancelled || (error as? URLError)?.code == .cancelled {
      // Tools that were not sent are recorded as skipped; nothing is re-sent later.
      for i in conversation.tools.indices where conversation.tools[i].state == .pending {
        conversation.tools[i].state = .skipped
      }
      try? conversation.save(storage)
      events(.error("cancelled"))
    } catch {
      events(.error("\(error)"))
    }
  }

  /// One streamed model response; transient provider errors are retried with
  /// backoff before any of its tool calls has been executed.
  private func modelTurn(_ events: @escaping @Sendable (AgentEvent) -> Void) async throws -> ([ContentBlock], String) {
    var attempt = 0
    while true {
      var text = ""
      var tools: [Int: (id: String, name: String, json: String)] = [:]
      var order: [Int] = []
      var stopReason = "end_turn"
      do {
        for try await e in try provider.stream(system: Self.systemPrompt, messages: wireMessages(), tools: self.tools) {
          switch e {
          case .text(let t):
            text += t
            events(.text(t))
          case .toolStart(let i, let id, let name):
            tools[i] = (id, name, "")
            order.append(i)
          case .toolInput(let i, let pj):
            tools[i]?.json += pj
          case .blockStop:
            break
          case .stop(let reason, let usage):
            stopReason = reason
            conversation.usage.inputTokens = usage.inputTokens
            conversation.usage.outputTokens += usage.outputTokens
            conversation.usage.cacheReadTokens += usage.cacheReadTokens
            conversation.usage.cacheWriteTokens += usage.cacheWriteTokens
          }
        }
        // A cancelled consumer sees the stream end normally; it is not an answer.
        try Task.checkCancellation()
        var blocks: [ContentBlock] = []
        if !text.isEmpty { blocks.append(.text(text)) }
        for i in order {
          guard let t = tools[i] else { continue }
          blocks.append(.toolUse(id: t.id, name: t.name, input: t.json.isEmpty ? "{}" : t.json))
        }
        return (blocks, stopReason)
      } catch let e as MessagesProvider.HTTPStatusError where e.retryable && attempt < 3 {
        attempt += 1
        // The provider's retry-after when it gave one (rate limits), else backoff.
        let wait = min(120, max(e.retryAfter ?? 0, 0.5 * Double(1 << attempt)))
        if !text.isEmpty { events(.discardText) }
        events(.retrying("provider returned \(e.status); retry \(attempt) in \(Int(wait.rounded(.up))) s"))
        try await Task.sleep(for: .seconds(wait))
        continue
      } catch let e as URLError where attempt < 3 && e.code != .cancelled {
        attempt += 1
        if !text.isEmpty { events(.discardText) }
        events(.retrying("connection to the model failed (\(e.code.rawValue)); retry \(attempt)"))
      }
      try await Task.sleep(for: .milliseconds(500 * (1 << attempt)))
    }
  }

  private func wireMessages() -> [ChatMessage] {
    guard let s = conversation.summary else { return conversation.messages }
    return [ChatMessage(role: "user", content: [.text("Summary of the earlier conversation:\n\(s)")])] + conversation.messages
  }

  /// The RPC tools and the Agent's own (reference_read, when references exist).
  public var tools: [ToolSpec] { references.isEmpty ? catalog.tools : catalog.tools + [Self.referenceTool] }

  static let referenceTool = ToolSpec(
    name: Shared.data.referenceTool.name, requestName: "", payloadType: 0, object: -1,
    description: Shared.data.referenceTool.description, inputSchema: Shared.data.referenceTool.inputSchema,
    mutating: false)

  /// The request that has the model implement Draft Cards (the same text as
  /// the web Agent's, from agent/Shared/agent.json).
  public static func draftRequest(scope: [String], cards: [(key: String, definition: String, description: String, content: String)]) -> String {
    let t = Shared.data.draft
    let list = cards.map {
      Shared.fill(t.card, ["key": $0.key, "definition": $0.definition, "description": $0.description, "content": $0.content])
    }.joined(separator: "\n")
    let where_ = scope.isEmpty ? t.root : Shared.fill(t.child, ["scope": scope.joined(separator: "/")])
    return Shared.fill(t.text, ["where": where_, "list": list])
  }

  /// The bundled demos the Agent may read, as references
  /// ("demos/<path>"): not the Draft demos (tasks to implement) nor the
  /// finished demo of each, which is that task's answer: an Agent that read
  /// it would copy it, and implementing the Draft would show nothing.
  public static func referenceDemos(in demos: URL) -> [String: URL] {
    guard let data = try? Data(contentsOf: demos.appendingPathComponent("index.json")),
          let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let list = obj["demos"] as? [[String: Any]] else { return [:] }
    let paths = list.compactMap { $0["path"] as? String }
    let tasks = Set(paths.filter { $0.hasSuffix("-draft") })
    var out: [String: URL] = [:]
    for p in paths where !tasks.contains(p) && !tasks.contains(p + "-draft") {
      out["demos/" + p] = demos.appendingPathComponent(p, isDirectory: true)
    }
    return out
  }

  /// Reads a bundled reference file or lists a folder.
  public func readReference(_ raw: String) -> (String, Bool) {
    let rel = raw.trimmingCharacters(in: CharacterSet(charactersIn: "/ ")).replacingOccurrences(of: "./", with: "")
    if rel.isEmpty || rel == "." {
      return (references.keys.sorted().map { references[$0]!.hasDirectoryPath ? $0 + "/" : $0 }.joined(separator: "\n"), false)
    }
    guard !rel.split(separator: "/").contains(".."),
          let (top, base) = references.first(where: { rel == $0.key || rel.hasPrefix($0.key + "/") }) else {
      // A folder that only holds references ("demos"): its entries.
      let inside = references.keys.filter { $0.hasPrefix(rel + "/") }.map { k in
        let rest = k.dropFirst(rel.count + 1)
        return rest.contains("/") || references[k]!.hasDirectoryPath ? String(rest.prefix { $0 != "/" }) + "/" : String(rest)
      }
      if !rel.split(separator: "/").contains(".."), !inside.isEmpty { return (Set(inside).sorted().joined(separator: "\n"), false) }
      return ("not found: \(rel) (reference_read \"\" lists what there is)", true)
    }
    let rest = rel == top ? "" : String(rel.dropFirst(top.count + 1))
    let url = rest.isEmpty ? base : base.appendingPathComponent(rest)
    // Never outside the reference (symbolic links included).
    guard url.resolvingSymlinksInPath().path.hasPrefix(base.resolvingSymlinksInPath().path) else { return ("not found: \(rel)", true) }
    var isDir: ObjCBool = false
    guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) else { return ("not found: \(rel)", true) }
    if isDir.boolValue {
      let names = (try? FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: [.isDirectoryKey])) ?? []
      return (names.filter { !$0.lastPathComponent.hasPrefix(".") }.map { u in
        ((try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false) ? u.lastPathComponent + "/" : u.lastPathComponent
      }.sorted().joined(separator: "\n"), false)
    }
    guard let data = try? Data(contentsOf: url) else { return ("cannot read \(rel)", true) }
    if data.count > 200_000 { return (String(decoding: data.prefix(200_000), as: UTF8.self) + "\n[cut at 200000 bytes]", false) }
    return (String(decoding: data, as: UTF8.self), false)
  }

  /// Validates arguments, executes the RPC and records the result.
  private func runTool(id: String, name: String, input: String, _ events: @escaping @Sendable (AgentEvent) -> Void) async -> ContentBlock {
    events(.toolStarted(id: id, name: name, arguments: input))
    var record = conversation.tools.first { $0.id == id } ?? ToolRecord(id: id, name: name, arguments: input, state: .pending)
    if let existing = conversation.tools.firstIndex(where: { $0.id == id }) { conversation.tools.remove(at: existing) }
    if name == Self.referenceTool.name, !references.isEmpty {
      let path = (try? JSONSerialization.jsonObject(with: Data(input.utf8)) as? [String: Any])?["path"] as? String ?? ""
      let (text, isError) = readReference(path)
      return finish(&record, text, isError: isError, events)
    }
    guard let spec = catalog.tool(named: name) else {
      return finish(&record, "unknown tool \(name)", isError: true, events)
    }
    // Incomplete or invalid arguments are never executed.
    guard let data = input.data(using: .utf8), let args = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else {
      return finish(&record, "the arguments are not a complete JSON object", isError: true, events)
    }
    var full = args
    // Fields the Agent fills itself: never the model's.
    for f in ToolCatalog.agentFields { full.removeValue(forKey: f) }
    if spec.mutating {
      record.opId = record.opId ?? UUID().uuidString.lowercased()
      full["op_id"] = record.opId
      full["origin"] = "agent"
    }
    // Validate by building once before sending anything.
    do {
      var probe = FlatBufferBuilder()
      _ = try codec.build(spec.object, full, into: &probe)
    } catch {
      return finish(&record, "\(error)", isError: true, events)
    }
    record.state = .sent
    conversation.tools.append(record)
    try? conversation.save(storage)
    do {
      let codec = self.codec
      var (replyType, table) = try await transport.call(payloadType: spec.payloadType) { b in
        try codec.build(spec.object, full, into: &b)
      }
      var json = renderReply(replyType, table)
      // A save that widens what Cards may do: the user approves it (the app
      // applies their defaults or asks), then it is sent again as a new
      // operation with the approvals; declined, nothing is saved.
      if replyType == "Failure", let asked = Self.approvalRequired(json) {
        let approved = await approver?(asked) ?? []
        guard Set(asked).isSubset(of: approved) else {
          let declined = asked.filter { !approved.contains($0) }.joined(separator: ", ")
          return finish(&record, Shared.fill(Shared.data.permissionDeclined, ["permissions": declined]), isError: true, events)
        }
        record.opId = UUID().uuidString.lowercased()
        full["op_id"] = record.opId
        full["approved_permissions"] = asked
        (replyType, table) = try await transport.call(payloadType: spec.payloadType) { b in
          try codec.build(spec.object, full, into: &b)
        }
        json = renderReply(replyType, table)
      }
      // A run still building or running: wait here (up to statusWait) instead
      // of the model spending a reply on each look.
      if name == "run_status" {
        let deadline = Date().addingTimeInterval(statusWait)
        while replyType == "RunStatus", let state = Self.runReply(json).state, Self.activeStates.contains(state),
              Date() < deadline {
          try await Task.sleep(for: .seconds(1))
          (replyType, table) = try await transport.call(payloadType: spec.payloadType) { b in
            try codec.build(spec.object, full, into: &b)
          }
          json = renderReply(replyType, table)
        }
      }
      noteRun(tool: name, reply: replyType, json: json)
      return finish(&record, json, isError: replyType == "Failure", events)
    } catch {
      // Unknown outcome: keep the record as sent so a resume asks OperationGet.
      if spec.mutating {
        return finish(&record, "the connection failed after sending; outcome unknown (op_id \(record.opId ?? "")). Use operation_get.",
                      isError: true, events, keepSent: true)
      }
      return finish(&record, "\(error)", isError: true, events)
    }
  }

  /// The permissions an approval_required Failure names.
  public static func approvalRequired(_ json: String) -> [String]? {
    guard let obj = try? JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any],
          let value = obj["value"] as? [String: Any], value["kind"] as? String == "approval_required" else { return nil }
    let args = value["args"] as? [[String: Any]] ?? []
    let asked = args.filter { $0["key"] as? String == "permission" }.compactMap { $0["value"] as? String }
    return asked.isEmpty ? nil : asked
  }

  /// Decides on the permissions a save would give Cards: returns those the
  /// user approves (by their defaults or when asked). Without one, nothing is.
  public typealias Approver = @Sendable ([String]) async -> Set<String>
  private var approver: Approver?
  public func setApprover(_ a: Approver?) { approver = a }

  static let activeStates: Set<String> = ["queued", "preparing", "running", "cancelling"]
  static let endedStates: Set<String> = ["succeeded", "failed", "cancelled"]

  /// The run id and state in a rendered RunAccepted / RunStatus reply
  /// (RunAccepted nests them in "run").
  static func runReply(_ json: String) -> (runId: String?, state: String?) {
    guard let obj = try? JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any],
          let value = obj["value"] as? [String: Any] else { return (nil, nil) }
    let run = value["run"] as? [String: Any] ?? value
    return (run["run_id"] as? String, run["state"] as? String)
  }

  /// Remembers a run the model started until it sees the run end.
  private func noteRun(tool: String, reply: String, json: String) {
    let (runId, state) = Self.runReply(json)
    if tool == "run", reply == "RunAccepted", let runId {
      unconfirmedRun = runId
    } else if reply == "RunStatus", let run = unconfirmedRun, runId == run, let state, Self.endedStates.contains(state) {
      unconfirmedRun = nil
    }
  }

  /// Every tool_use needs its tool_result at the start of the next message. A
  /// turn stopped between them (cancelled during a tool, a crash) gets the
  /// results recorded so far and "not run" for the rest, so the next request
  /// (a retry, a new message) is valid.
  func closeDanglingToolUses() {
    var i = 0
    var changed = false
    while i < conversation.messages.count {
      let m = conversation.messages[i]
      let ids = m.role == "assistant" ? m.content.compactMap { b -> String? in
        if case .toolUse(let id, _, _) = b { return id }
        return nil
      } : []
      let next = i + 1 < conversation.messages.count ? conversation.messages[i + 1] : nil
      let answered = Set(next?.role == "user" ? next!.content.compactMap { b -> String? in
        if case .toolResult(let id, _, _) = b { return id }
        return nil
      } : [])
      let missing = ids.filter { !answered.contains($0) }
      if !missing.isEmpty {
        let results = missing.map { id -> ContentBlock in
          let r = conversation.tools.first { $0.id == id }
          switch r?.state {
          case .completed?:
            return .toolResult(toolUseId: id, content: Self.clip(r?.result ?? ""), isError: r?.isError ?? false)
          case .sent?:
            return .toolResult(toolUseId: id, content: "sent, but the turn was stopped before the answer; check the outcome "
                                 + "with operation_get (op_id \(r?.opId ?? ""))", isError: true)
          default:
            return .toolResult(toolUseId: id, content: "not run: the turn was stopped before this call", isError: true)
          }
        }
        if next?.role == "user" {
          // Tool results come first in their message.
          conversation.messages[i + 1].content = results + conversation.messages[i + 1].content
        } else {
          conversation.messages.insert(ChatMessage(role: "user", content: results), at: i + 1)
        }
        for id in missing {
          if let j = conversation.tools.firstIndex(where: { $0.id == id }), conversation.tools[j].state == .pending {
            conversation.tools[j].state = .skipped
          }
        }
        changed = true
      }
      i += 1
    }
    if changed { try? conversation.save(storage) }
  }

  /// How much of a tool's result the model gets; a longer one says it was cut.
  public static let resultLimit = 60_000
  public static func clip(_ s: String) -> String {
    guard s.count > resultLimit else { return s }
    return String(s.prefix(resultLimit)) + "\n[cut: showing the first \(resultLimit) of \(s.count) characters; "
      + "read the rest with a narrower request (an offset, a range or one file)]"
  }

  private func finish(_ record: inout ToolRecord, _ result: String, isError: Bool,
                      _ events: @escaping @Sendable (AgentEvent) -> Void, keepSent: Bool = false) -> ContentBlock {
    record.state = keepSent ? .sent : .completed
    record.result = result
    record.isError = isError
    conversation.tools.removeAll { $0.id == record.id }
    conversation.tools.append(record)
    try? conversation.save(storage)
    events(.toolFinished(id: record.id, name: record.name, ok: !isError, summary: String(result.prefix(400))))
    return .toolResult(toolUseId: record.id, content: Self.clip(result), isError: isError)
  }

  private func renderReply(_ type: String, _ table: Table) -> String {
    let objName = type == "Failure" ? "Tsugumi.Data.Failure" : "Tsugumi.RPC.\(type)"
    guard let (i, _) = catalog.schema.object(named: objName) else { return "{\"reply\":\"\(type)\"}" }
    let obj: [String: Any] = ["reply": type, "value": codec.json(i, table)]
    let data = (try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys])) ?? Data()
    return String(decoding: data, as: UTF8.self)
  }

  /// Tool calls left "sent" (disconnect or restart) are checked with
  /// OperationGet; nothing non-idempotent is re-sent blindly.
  private func reconcilePending(_ events: @escaping @Sendable (AgentEvent) -> Void) async throws {
    guard let opSpec = catalog.tool(named: "operation_get") else { return }
    for i in conversation.tools.indices where conversation.tools[i].state == .sent {
      guard let op = conversation.tools[i].opId else { continue }
      let codec = self.codec
      if let (type, table) = try? await transport.call(payloadType: opSpec.payloadType, build: { b in
        try codec.build(opSpec.object, ["op_id": op], into: &b)
      }) {
        conversation.tools[i].state = .completed
        conversation.tools[i].result = renderReply(type, table)
        events(.toolFinished(id: conversation.tools[i].id, name: conversation.tools[i].name, ok: true,
                             summary: "reconciled with operation_get"))
      }
    }
    try? conversation.save(storage)
  }

  /// Context compaction: older turns are replaced by a model-written summary
  /// when the last request approached the provider's context limit.
  private func compactIfNeeded(_ events: @escaping @Sendable (AgentEvent) -> Void, force: Bool = false) async throws {
    let limit = provider.config.contextLimit
    guard force || conversation.usage.inputTokens > limit * 3 / 4, conversation.messages.count > (force ? 4 : 6) else { return }
    let keep = 4
    let old = Array(conversation.messages.dropLast(keep))
    var transcript = ""
    for m in old {
      for b in m.content {
        switch b {
        case .text(let t): transcript += "\(m.role): \(t)\n"
        case .toolUse(_, let n, let a): transcript += "tool call \(n) \(a.prefix(500))\n"
        case .toolResult(_, let c, let e): transcript += "tool result\(e ? " (error)" : ""): \(c.prefix(800))\n"
        }
      }
    }
    let ask = [ChatMessage(role: "user", content: [.text("Summarize this Tsugumi session so far in under 200 words, keeping revisions, run ids and decisions:\n\n\(transcript)")])]
    var summary = ""
    for try await e in try provider.stream(system: "You summarize conversations.", messages: ask, tools: []) {
      if case .text(let t) = e { summary += t }
    }
    // The kept tail must start with a user message that is not a tool result.
    var tail = Array(conversation.messages.suffix(keep))
    while let first = tail.first, first.role != "user" || first.content.contains(where: { if case .toolResult = $0 { return true }; return false }) {
      tail.removeFirst()
    }
    conversation.summary = [conversation.summary, summary].compactMap { $0 }.joined(separator: "\n")
    conversation.archive += conversation.messages.dropLast(tail.count)
    conversation.messages = tail
    conversation.usage.inputTokens = 0
    try? conversation.save(storage)
    events(.compacted)
  }
}
