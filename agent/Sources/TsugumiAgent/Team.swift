import Foundation

/// `delegate`: the main Agent hands independent parts of the work to helper
/// Agents that work at the same time in the same project, each with its own
/// conversation and the same tools. A helper owns the Cards of its task: its
/// saves may change only their definitions, so helpers never overwrite each
/// other, and the meeting of their saves and runs (busy, revision_conflict)
/// is resolved by the harness, not by the models.
extension Agent {
  /// A helper's name, the root-level Cards it owns and its task.
  public struct Helper: Sendable, Equatable {
    public let name: String
    public let cards: [String]
    public let instruction: String

    public init(name: String, cards: [String], instruction: String) {
      self.name = name
      self.cards = cards
      self.instruction = instruction
    }

    /// What a save's changes touch beyond the helper's Cards (nil: nothing).
    func outside(_ changes: Any?) -> String? {
      guard let list = changes as? [[String: Any]] else { return nil }
      var out: [String] = []
      for c in list {
        let kind = c["value_type"] as? String ?? "?"
        let value = c["value"] as? [String: Any] ?? [:]
        switch kind {
        case "PutDefinition":
          let id = value["id"] as? String ?? ""
          if !cards.contains(id) { out.append("the definition \(id)") }
        case "PutSource", "RemoveSource":
          let path = value["path"] as? String ?? ""
          let parts = path.split(separator: "/").map(String.init)
          if !(parts.count >= 3 && parts[0] == "definitions" && cards.contains(parts[1]) && !parts.contains("..")) {
            out.append(path.isEmpty ? kind : path)
          }
        default:
          out.append(kind)
        }
      }
      return out.isEmpty ? nil : out.joined(separator: ", ")
    }
  }

  /// How long a helper's save or run waits for the others' (busy).
  static let helperWait: TimeInterval = 900
  /// Model replies with tool calls a helper may take.
  static let helperSteps = 40

  static let delegateTool = ToolSpec(
    name: Shared.data.delegateTool.name, requestName: "", payloadType: 0, object: -1,
    description: Shared.data.delegateTool.description, inputSchema: Shared.data.delegateTool.inputSchema,
    mutating: false)

  /// The rules, and for a helper its task.
  var system: String {
    if role == .guide { return Self.guidePrompt }
    guard let helper else { return Self.systemPrompt }
    return Self.systemPrompt + "\n\n" + Shared.fill(Shared.data.helper.prompt, [
      "name": helper.name, "cards": helper.cards.joined(separator: ", "), "instruction": helper.instruction])
  }

  /// The tasks of a delegate call, or why they cannot be done.
  static func tasks(_ input: String) -> Result<[Helper], DelegateError> {
    guard let obj = try? JSONSerialization.jsonObject(with: Data(input.utf8)) as? [String: Any],
          let raw = obj["tasks"] as? [[String: Any]] else { return .failure(.init("tasks is not a list of tasks")) }
    guard (2...8).contains(raw.count) else { return .failure(.init("\(raw.count) tasks")) }
    var helpers: [Helper] = []
    var owner: [String: String] = [:]
    for t in raw {
      let name = (t["name"] as? String ?? "").trimmingCharacters(in: .whitespaces)
      let cards = (t["cards"] as? [String] ?? []).map { $0.trimmingCharacters(in: .whitespaces) }
      let instruction = t["instruction"] as? String ?? ""
      guard !name.isEmpty, name.count <= 40, name.allSatisfy({ $0.isLetter || $0.isNumber || "-_".contains($0) }) else {
        return .failure(.init("the name \"\(name)\" (letters, digits, - and _)"))
      }
      guard !helpers.contains(where: { $0.name == name }) else { return .failure(.init("two tasks named \(name)")) }
      guard !cards.isEmpty, !instruction.isEmpty else { return .failure(.init("\(name) has no Cards or no instruction")) }
      for c in cards {
        guard !c.isEmpty, !c.contains("/") else { return .failure(.init("\(c) is not a root-level Card id")) }
        if let other = owner[c] { return .failure(.init("\(c) is in both \(other) and \(name)")) }
        owner[c] = name
      }
      helpers.append(Helper(name: name, cards: cards, instruction: instruction))
    }
    return .success(helpers)
  }

  public struct DelegateError: Error {
    let why: String
    init(_ why: String) { self.why = why }
  }

  /// Runs the helpers of a delegate call at the same time and returns their
  /// reports (an error only when the call itself is wrong or none finished).
  /// A conversation that cannot be saved stops the work (as for one Agent).
  func delegate(_ input: String, _ events: @escaping @Sendable (AgentEvent) -> Void) async throws -> (String, Bool) {
    let helpers: [Helper]
    switch Self.tasks(input) {
    case .success(let h): helpers = h
    case .failure(let e): return (Shared.fill(Shared.data.helper.invalid, ["why": e.why]), true)
    }
    // Each helper keeps its conversation next to this one's (for reading
    // afterwards); a new delegate call starts them afresh.
    let dir = storage.deletingPathExtension().appendingPathExtension("helpers")
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let line = ApprovalLine(approverForHelpers)
    let (catalog, transport, config, references) = (self.catalog, self.transport, self.providerConfig, self.references)
    let reports = await withTaskGroup(of: (Int, HelperReport).self) { group in
      for (i, h) in helpers.enumerated() {
        let file = dir.appendingPathComponent(h.name + ".json")
        try? FileManager.default.removeItem(at: file)
        guard let agent = try? Agent(catalog: catalog, transport: transport, provider: config, storage: file,
                                     references: references, helper: h) else {
          group.addTask { (i, HelperReport(outcome: "failed: its conversation cannot be read at \(file.path)", report: "", usage: Usage())) }
          continue
        }
        group.addTask {
          await agent.setMaxSteps(Self.helperSteps)
          await agent.setApprover { asked in await line.ask(asked) }
          return (i, await agent.work { e in events(.helper(name: h.name, event: e)) })
        }
      }
      var out = [HelperReport?](repeating: nil, count: helpers.count)
      for await (i, r) in group { out[i] = r }
      return out.compactMap { $0 }
    }
    for r in reports {
      conversation.usage.outputTokens += r.usage.outputTokens
      conversation.usage.cacheReadTokens += r.usage.cacheReadTokens
      conversation.usage.cacheWriteTokens += r.usage.cacheWriteTokens
    }
    try conversation.save(storage)
    let text = zip(helpers, reports).map { h, r in
      "## \(h.name) (\(h.cards.joined(separator: ", "))): \(r.outcome)\n\(r.report.isEmpty ? "(no report)" : r.report)"
    }.joined(separator: "\n\n")
    let none = !reports.contains { $0.outcome == "finished" }
    return ("Reports of the helpers. Check each Card with run_record before reporting to the user.\n\n" + text, none)
  }

  struct HelperReport: Sendable {
    let outcome: String
    let report: String
    let usage: Usage
  }

  /// A helper's whole task in one turn: its last words are its report.
  func work(events: @escaping @Sendable (AgentEvent) -> Void) async -> HelperReport {
    conversation = Conversation()
    conversation.title = helper?.name
    conversation.messages.append(ChatMessage(role: "user", content: [.text("Do your task (in the rules above) and end with your report.")]))
    do { try conversation.save(storage) } catch {
      events(.error("\(error)"))
      return HelperReport(outcome: "failed: \(error)", report: "", usage: conversation.usage)
    }
    let outcome = Outcome()
    await loop { e in
      switch e {
      case .finished: outcome.set("finished")
      case .stepLimit(let n, _): outcome.set("stopped after \(n) steps")
      case .error(let s): outcome.set(s == "cancelled" ? "cancelled" : "failed: \(s)")
      default: break
      }
      events(e)
    }
    let last = conversation.messages.last { $0.role == "assistant" }
    let report = last?.content.compactMap { b -> String? in if case .text(let t) = b { return t }; return nil }
      .joined(separator: "\n") ?? ""
    return HelperReport(outcome: outcome.value, report: report, usage: conversation.usage)
  }

  /// `kind` of a rendered Failure.
  static func failureKind(_ json: String) -> String? {
    guard let obj = try? JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any],
          obj["reply"] as? String == "Failure", let value = obj["value"] as? [String: Any] else { return nil }
    return value["kind"] as? String
  }

  /// The revision a revision_conflict Failure names as current.
  static func currentRev(_ json: String) -> UInt64? {
    guard let obj = try? JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any],
          let value = obj["value"] as? [String: Any] else { return nil }
    if let s = value["current_rev"] as? String { return UInt64(s) }
    return (value["current_rev"] as? NSNumber)?.uint64Value
  }
}

/// How a helper's turn ended (set from its events).
private final class Outcome: @unchecked Sendable {
  private let lock = NSLock()
  private var v = "stopped"
  func set(_ s: String) { lock.withLock { v = s } }
  var value: String { lock.withLock { v } }
}

/// The user answers one approval at a time, whichever helper asks.
actor ApprovalLine {
  private let approver: Agent.Approver?
  private var tail: Task<Set<String>, Never>?

  init(_ approver: Agent.Approver?) { self.approver = approver }

  func ask(_ asked: [String]) async -> Set<String> {
    guard let approver else { return [] }
    let previous = tail
    let next = Task { _ = await previous?.value; return await approver(asked) }
    tail = next
    return await next.value
  }
}

/// Going on after a revision_conflict that only others' layout changes
/// caused (the canvas tidied while the Agent worked, a Card dragged).
extension Agent {
  /// Remembers the definitions a reply shows (an inspect's, a save's).
  func noteDefinitions(reply: String, json: String) {
    guard reply == "ProjectSnapshot" || reply == "SaveResult",
          let obj = try? JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any],
          let value = obj["value"] as? [String: Any] else { return }
    if reply == "ProjectSnapshot" {
      seenDefinitions = [:]
      for d in value["definitions"] as? [[String: Any]] ?? [] {
        if let id = d["id"] as? String { seenDefinitions[id] = d["card_json"] as? String ?? "" }
      }
    } else if let delta = value["delta"] as? [String: Any] {
      for d in delta["definitions_upsert"] as? [[String: Any]] ?? [] {
        if let id = d["id"] as? String { seenDefinitions[id] = d["card_json"] as? String ?? "" }
      }
      for id in delta["definitions_removed"] as? [String] ?? [] { seenDefinitions[id] = nil }
    }
  }

  /// The current revision, when sending again changes nothing of what others
  /// did: a save that rewrites only definitions (and files, which the core
  /// checks by their base_hash) still as this Agent saw them; a run when no
  /// definition it saw has changed. nil: the model sees the conflict.
  func rebased(tool: String, args: [String: Any]) async -> UInt64? {
    guard tool == "project_save" || tool == "run", !seenDefinitions.isEmpty,
          let inspect = catalog.tool(named: "project_inspect") else { return nil }
    var ids: [String]
    if tool == "project_save" {
      guard let changes = args["changes"] as? [[String: Any]] else { return nil }
      ids = []
      for c in changes {
        switch c["value_type"] as? String {
        case "PutDefinition": ids.append((c["value"] as? [String: Any])?["id"] as? String ?? "")
        case "PutSource": break  // the core checks the file by its base_hash
        default: return nil  // the document, the layout, removals and renames: the model decides
        }
      }
    } else {
      ids = Array(seenDefinitions.keys)
    }
    let codec = self.codec
    guard let (type, table) = try? await transport.call(payloadType: inspect.payloadType, build: { b in
      try codec.build(inspect.object, [:], into: &b)
    }), type == "ProjectSnapshot",
      let obj = try? JSONSerialization.jsonObject(with: Data(renderReply(type, table).utf8)) as? [String: Any],
      let value = obj["value"] as? [String: Any] else { return nil }
    var now: [String: String] = [:]
    for d in value["definitions"] as? [[String: Any]] ?? [] {
      if let id = d["id"] as? String { now[id] = d["card_json"] as? String ?? "" }
    }
    for id in ids where now[id] != seenDefinitions[id] { return nil }
    if let s = value["rev"] as? String { return UInt64(s) }
    return (value["rev"] as? NSNumber)?.uint64Value
  }
}
