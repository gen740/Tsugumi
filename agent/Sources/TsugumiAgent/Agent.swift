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

  public static func load(_ url: URL) -> Conversation {
    (try? JSONDecoder().decode(Conversation.self, from: Data(contentsOf: url))) ?? Conversation()
  }

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
  case error(String)
  case finished
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

  public static let systemPrompt = """
  You operate Tsugumi, a Card graph tool, through its core RPC tools. Rules:
  - Call project_inspect first; use its rev as base_rev/at_rev. Revisions change after every save.
  - project_save takes changes as a list of {"value_type": "SetProject", "value": {"json": "<entire tsugumi.json>"}};
    SetProject replaces the whole document, so start from the project_json you inspected and change only what is needed.
  - Never save edits the user did not ask for. If a save returns revision_conflict, inspect again and re-apply.
  - Run with {"base_rev": ..., "card": {"segments": ["<trigger id>"]}}; then poll run_status with the run_id until
    succeeded/failed/cancelled and read results with run_record. Cancelling runs requires an explicit run_cancel.
  - Definitions and sources change in the same project_save: {"value_type": "PutDefinition", "value": {"id": "...",
    "card_json": "<_card.json>", "meta_json": "<_meta.json>"}}, {"value_type": "PutSource", "value": {"path":
    "definitions/<id>/<file>", "bytes": "<file text>", "base_hash": "<hash when replacing>"}}, RemoveDefinition.
    One save is one transaction: either everything is stored or nothing is.
  - Implementing Drafts: a Draft (template "draft") states its purpose in _meta.json (description, plan,
    constraints, done, inputs/outputs with descriptions). Turn it into code Cards: set "language" (python, cpp or
    bash) and remove "template" in _card.json, write _outputs.fbs (one table per output port, root table Outputs),
    default.nix ({ pkgs, inputs, params }: { package = inputs.tsugumi.pythonCard { src = ./src; }; } or cppCard,
    or script for bash) and the sources. Input types come from the connected outputs; never write _inputs.fbs.
    Keep the instance ids and wiring; you may split one Draft into several Cards (rewire them in SetProject).
    Keep the intent and the port descriptions in _meta.json. Then project_validate, fix what it reports, run the
    workflow and check the result. If something fails, report it; never leave a half-written save.
  - 64 bit integers are decimal strings. Explain results briefly in the user's language.
  """

  public init(catalog: ToolCatalog, transport: ToolTransport, provider: ProviderConfig, storage: URL) {
    self.catalog = catalog
    self.codec = FlatJSON(schema: catalog.schema)
    self.transport = transport
    self.provider = MessagesProvider(config: provider)
    self.storage = storage
    self.conversation = Conversation.load(storage)
  }

  public func setProvider(_ config: ProviderConfig) { provider = MessagesProvider(config: config) }
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
    turn = nil
  }

  public func send(_ text: String, context: [String], events: @escaping @Sendable (AgentEvent) -> Void) {
    turn?.cancel()
    var prompt = text
    if !context.isEmpty { prompt += "\n\n[Context: selected Cards \(context.joined(separator: ", "))]" }
    send(prompt: prompt, title: text, events: events)
  }

  /// Sends a prepared prompt (references and skills already attached).
  public func send(prompt: String, title: String, events: @escaping @Sendable (AgentEvent) -> Void) {
    turn?.cancel()
    if conversation.title == nil { conversation.title = String(title.prefix(60)) }
    conversation.messages.append(ChatMessage(role: "user", content: [.text(prompt)]))
    conversation.updatedAt = Date()
    try? conversation.save(storage)
    turn = Task { await self.loop(events) }
  }

  /// Retries after a failed or stopped answer. A turn whose model reply never
  /// arrived is continued; otherwise the last request is asked again. Tool
  /// calls already confirmed by the core are reconciled, never re-sent.
  public func retry(events: @escaping @Sendable (AgentEvent) -> Void) {
    turn?.cancel()
    if let last = conversation.messages.last, last.role == "user" {
      turn = Task { await self.loop(events) }
      return
    }
    let lastText = conversation.messages.last { m in
      m.role == "user" && m.content.contains { if case .text = $0 { return true }; return false }
    }
    guard let lastText else { return }
    conversation.messages.append(lastText)
    conversation.updatedAt = Date()
    try? conversation.save(storage)
    turn = Task { await self.loop(events) }
  }

  /// Compacts the context now (the full history stays in the archive).
  public func compact(events: @escaping @Sendable (AgentEvent) -> Void) {
    turn?.cancel()
    turn = Task {
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
    turn = Task { await self.loop(events) }
  }

  private func loop(_ events: @escaping @Sendable (AgentEvent) -> Void) async {
    do {
      try await reconcilePending(events)
      for _ in 0..<24 {
        try Task.checkCancellation()
        try await compactIfNeeded(events)
        let (blocks, stop) = try await modelTurn(events)
        conversation.messages.append(ChatMessage(role: "assistant", content: blocks))
        try? conversation.save(storage)
        let uses = blocks.compactMap { b -> (String, String, String)? in
          if case .toolUse(let id, let name, let input) = b { return (id, name, input) }
          return nil
        }
        if uses.isEmpty || stop != "tool_use" { break }
        var results: [ContentBlock] = []
        for (id, name, input) in uses {
          try Task.checkCancellation()
          results.append(await runTool(id: id, name: name, input: input, events))
        }
        conversation.messages.append(ChatMessage(role: "user", content: results))
        try? conversation.save(storage)
      }
      events(.finished)
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
        for try await e in try provider.stream(system: Self.systemPrompt, messages: wireMessages(), tools: catalog.tools) {
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
        events(.retrying("provider returned \(e.status); retry \(attempt)"))
      } catch let e as URLError where attempt < 3 && e.code != .cancelled {
        attempt += 1
        events(.retrying("connection to the model failed (\(e.code.rawValue)); retry \(attempt)"))
      }
      try await Task.sleep(for: .milliseconds(500 * (1 << attempt)))
    }
  }

  private func wireMessages() -> [ChatMessage] {
    guard let s = conversation.summary else { return conversation.messages }
    return [ChatMessage(role: "user", content: [.text("Summary of the earlier conversation:\n\(s)")])] + conversation.messages
  }

  /// Validates arguments, executes the RPC and records the result.
  private func runTool(id: String, name: String, input: String, _ events: @escaping @Sendable (AgentEvent) -> Void) async -> ContentBlock {
    events(.toolStarted(id: id, name: name, arguments: input))
    var record = conversation.tools.first { $0.id == id } ?? ToolRecord(id: id, name: name, arguments: input, state: .pending)
    if let existing = conversation.tools.firstIndex(where: { $0.id == id }) { conversation.tools.remove(at: existing) }
    guard let spec = catalog.tool(named: name) else {
      return finish(&record, "unknown tool \(name)", isError: true, events)
    }
    // Incomplete or invalid arguments are never executed.
    guard let data = input.data(using: .utf8), let args = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else {
      return finish(&record, "the arguments are not a complete JSON object", isError: true, events)
    }
    var full = args
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
      let (replyType, table) = try await transport.call(payloadType: spec.payloadType) { b in
        try codec.build(spec.object, full, into: &b)
      }
      let json = renderReply(replyType, table)
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

  private func finish(_ record: inout ToolRecord, _ result: String, isError: Bool,
                      _ events: @escaping @Sendable (AgentEvent) -> Void, keepSent: Bool = false) -> ContentBlock {
    record.state = keepSent ? .sent : .completed
    record.result = result
    record.isError = isError
    conversation.tools.removeAll { $0.id == record.id }
    conversation.tools.append(record)
    try? conversation.save(storage)
    events(.toolFinished(id: record.id, name: record.name, ok: !isError, summary: String(result.prefix(400))))
    return .toolResult(toolUseId: record.id, content: String(result.prefix(60_000)), isError: isError)
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
