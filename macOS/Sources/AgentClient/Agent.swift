import FlatBuffers
import Foundation
import TsugumiRPC

/// Supplies a project endpoint to the core-owned Agent service. No tool call
/// is executed by the application.
public protocol ToolTransport: Sendable {
  func project() async throws -> AgentProject?
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

/// A snapshot of a conversation persisted by the core Agent service.
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


}

public enum AgentEvent: Sendable {
  case text(String)
  /// A piece of a tool call's arguments as the model writes them; the call
  /// has not started, and may never (`toolStarted` says when it does).
  case toolInput(index: Int, name: String, delta: String)
  case toolStarted(id: String, name: String, arguments: String)
  case toolFinished(id: String, name: String, ok: Bool, summary: String)
  /// Earlier turns are being summarized: the next reply waits for it.
  case compacting
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
  /// What a helper started by `delegate` does (its own events).
  indirect case helper(name: String, event: AgentEvent)
}


/// A UI handle to a durable conversation owned by core. All model calls,
/// tools, persistence and authorization run behind the Agent socket.
public actor Agent {
  public enum Role: String, Sendable { case worker, guide }
  public typealias Approver = @Sendable ([String]) async -> Set<String>
  public let id: String
  public private(set) var conversation = Conversation()
  private let transport: ToolTransport
  private let role: Role
  private var provider: ProviderConfig
  private var approver: Approver?
  private var observation: Task<Void, Never>?
  private var transition: Task<Void, Error>?
  private var sequence: UInt64 = 0
  private var opened = false
  private var busy = false
  public private(set) var canContinue = false
  private var resumable = false
  private var answeredAccess: [String: Bool] = [:]
  private let client = CoreAgentClient.shared

  public static let guideScript = Shared.guideScript
  public static let guideScriptEnglish = Shared.guideScriptEnglish
  public static let contextFullText = "the conversation no longer fits the model's context"

  public init(id: String, transport: ToolTransport, provider: ProviderConfig, role: Role = .worker) {
    self.id = id
    self.transport = transport
    self.provider = provider
    self.role = role
    conversation.id = id
  }

  public func setProvider(_ config: ProviderConfig) { provider = config }
  public func setApprover(_ handler: Approver?) { approver = handler }

  public func open() async throws {
    var fields: [String: Any] = ["conversation_id": id, "role": role.rawValue]
    if let project = try await transport.project() {
      fields["project_id"] = project.id
    }
    let result = try await client.call("ConversationCreateRequest", fields)
    try adopt(result)
    if !opened { sequence = result["event_seq"] as? UInt64 ?? 0 }
    opened = true
    try await resumeInterrupted()
  }

  private func adopt(_ result: [String: Any]) throws {
    if let value = result["conversation"] as? Tsugumi_Agent_ConversationT { conversation = try ServiceWire.conversation(value) }
    busy = result["busy"] as? Bool ?? false
    resumable = result["resumable"] as? Bool ?? false
    canContinue = result["continuable"] as? Bool ?? false
  }

  public func refresh() async throws {
    if !opened { return try await open() }
    try adopt(try await client.call("ConversationGetRequest", ["conversation_id": id]))
    try await resumeInterrupted()
  }

  private func resumeInterrupted() async throws {
    if !busy && resumable {
      do { _ = try await client.call("ConversationActionRequest", ["conversation_id": id, "action": "Resume"]) }
      catch let error as CoreAgentFailure where error.code == "busy" {}
      busy = true
      resumable = false
      canContinue = false
    }
  }

  /// Leaving a window detaches observation; it does not stop server work.
  public func detach() { observation?.cancel(); observation = nil }

  public func cancel() async {
    observation?.cancel()
    observation = nil
    let previous = transition
    previous?.cancel()
    let stopping = Task {
      _ = try? await previous?.value
      _ = try await client.call("ConversationCancelRequest", ["conversation_id": id])
      while true {
        let result = try await client.call("ConversationGetRequest", ["conversation_id": id])
        try adopt(result)
        if !busy {
          sequence = result["event_seq"] as? UInt64 ?? sequence
          break
        }
        try await Task.sleep(for: .milliseconds(20))
      }
    }
    transition = stopping
    _ = try? await stopping.value
  }

  private func beginTurn() async throws {
    let result = try await client.call("ConversationGetRequest", ["conversation_id": id])
    try Task.checkCancellation()
    try adopt(result)
    sequence = result["event_seq"] as? UInt64 ?? sequence
  }

  public func reset() async throws {
    _ = try await client.call("ConversationActionRequest", ["conversation_id": id, "action": "Reset"])
    try await refresh()
  }

  public var hasUnfinishedWork: Bool { busy }

  public func send(prompt: String, title: String, display: String? = nil,
                   selectedCards: [String] = [], selectedDisplays: [String] = [], skillNames: [String] = [], attachedResources: [String] = [],
                   events: @escaping @Sendable (AgentEvent) -> Void) {
    let turn = UUID().uuidString
    start(events) {
      try await self.open()
      try await self.client.configure(self.provider, conversationID: self.id)
      try await self.beginTurn()
      _ = try await self.client.call("ConversationSendRequest", ["conversation_id": self.id,
        "turn_id": turn, "text": prompt, "selected_cards": selectedCards,
        "selected_displays": selectedDisplays, "skill_names": skillNames, "attached_resources": attachedResources])
    }
  }

  public func retry(events: @escaping @Sendable (AgentEvent) -> Void) { action("Retry", events) }
  public func continueAfterLimit(events: @escaping @Sendable (AgentEvent) -> Void) { action("Continue", events) }
  public func compact(events: @escaping @Sendable (AgentEvent) -> Void) { action("Compact", events) }
  public func resume(events: @escaping @Sendable (AgentEvent) -> Void) { start(events) { try await self.open() } }

  private func action(_ name: String, _ events: @escaping @Sendable (AgentEvent) -> Void) {
    start(events) {
      try await self.open()
      try await self.client.configure(self.provider, conversationID: self.id)
      try await self.beginTurn()
      _ = try await self.client.call("ConversationActionRequest", ["conversation_id": self.id, "action": name])
    }
  }

  private func start(_ events: @escaping @Sendable (AgentEvent) -> Void, operation: @escaping () async throws -> Void) {
    observation?.cancel()
    let previous = transition
    let accepted = Task {
      _ = try? await previous?.value
      try Task.checkCancellation()
      try await operation()
    }
    transition = accepted
    observation = Task {
      do {
        try await accepted.value
        try Task.checkCancellation()
        let access = try await client.call("AccessListRequest", ["conversation_id": id])
        try Task.checkCancellation()
        for request in access["requests"] as? [[String: Any]] ?? [] where request["state"] as? String == "pending" {
          guard let requestID = request["id"] as? String else { continue }
          let resource = request["project_id"] as? String ?? ""
          let capability = request["access"] as? String ?? "Read"
          let reason = request["reason"] as? String ?? ""
          let scope = request["scope"] as? String ?? "Conversation"
          _ = try await answerAccess(requestID, asked: ["project_access \(resource): \(capability) [\(scope)] — \(reason)"])
        }
        while !Task.isCancelled {
          let result = try await client.call("EventsRequest", ["conversation_id": id, "after_seq": sequence])
          try Task.checkCancellation()
          for item in result["events"] as? [[String: Any]] ?? [] {
            sequence = max(sequence, item["seq"] as? UInt64 ?? sequence)
            guard let event = item["event"] as? [String: Any] else { continue }
            if event["kind"] as? String == "permission", let request = event["id"] as? String {
              let asked = event["asked"] as? [String] ?? ["project_access Project content"]
              _ = try await answerAccess(request, asked: asked)
            } else if let event = Self.event(event) {
              if case .finished = event { try await refresh() }
              try Task.checkCancellation()
              events(event)
            }
          }
          sequence = max(sequence, result["next_seq"] as? UInt64 ?? sequence)
          busy = result["busy"] as? Bool ?? false
          if !busy { try await refresh(); break }
          try await Task.sleep(for: .milliseconds(80))
        }
      } catch is CancellationError {} catch {
        if !Task.isCancelled { events(.error(String(describing: error))) }
      }
    }
  }

  private func answerAccess(_ request: String, asked: [String]) async throws -> Bool {
    if let answer = answeredAccess[request] { return answer }
    let access = try await client.call("AccessListRequest", ["conversation_id": id])
    if let state = (access["requests"] as? [[String: Any]])?.first(where: { $0["id"] as? String == request })?["state"] as? String,
       state != "pending" {
      answeredAccess[request] = state == "allowed"
      return state == "allowed"
    }
    guard let approver else { throw CancellationError() }
    let answer = await approver(asked)
    try Task.checkCancellation()
    let allowed = Set(asked).isSubset(of: answer)
    do {
      _ = try await client.call("AccessResolveRequest", ["request_id": request, "allow": allowed])
    } catch let error as CoreAgentFailure where error.code == "conflict" {
      // A human in another UI already answered; read the authoritative outcome.
      return try await answerAccess(request, asked: asked)
    }
    answeredAccess[request] = allowed
    return allowed
  }

  private static func event(_ value: [String: Any]) -> AgentEvent? {
    let text = value["text"] as? String ?? ""
    let name = value["name"] as? String ?? ""
    let id = value["id"] as? String ?? ""
    switch value["kind"] as? String {
    case "text": return .text(text)
    case "toolInput": return .toolInput(index: value["index"] as? Int ?? 0, name: name, delta: value["delta"] as? String ?? text)
    case "toolStarted": return .toolStarted(id: id, name: name, arguments: value["arguments"] as? String ?? text)
    case "toolFinished": return .toolFinished(id: id, name: name, ok: value["ok"] as? Bool ?? false, summary: value["summary"] as? String ?? text)
    case "compacting": return .compacting
    case "compacted": return .compacted
    case "retrying": return .retrying(value["message"] as? String ?? text)
    case "discardText": return .discardText
    case "error": return .error(value["message"] as? String ?? text)
    case "finished", "cancelled": return .finished
    case "stepLimit": return .stepLimit(steps: value["steps"] as? Int ?? 0, unconfirmedRun: value["unconfirmedRun"] as? String)
    case "helper":
      guard let nested = value["event"] as? [String: Any], let event = event(nested) else { return nil }
      return .helper(name: name, event: event)
    default: return nil
    }
  }

  public static func guideText(_ key: String, _ values: [String: String] = [:]) -> String {
    Shared.fill(Shared.data.guide.text(key), values)
  }

  public static func toolSummary(_ result: String) -> String { String(result.prefix(400)) }
}
