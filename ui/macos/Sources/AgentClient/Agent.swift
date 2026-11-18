import FlatBuffers
import Foundation
import TsugumiRPC

/// Supplies a project endpoint to the core-owned Agent service. No tool call
/// is executed by the application.
public protocol ToolTransport: Sendable {
  func project() async throws -> AgentProject?
}

/// Presentation assembled by hostd from structured messages and task records.
public struct ChatRow: Codable, Equatable, Sendable, Identifiable {
  public var id: String
  public var kind: String
  public var role: String
  public var text: String
  public var name: String
  public var arguments: String
  public var result: String
  public var state: String
  public var archived: Bool
  public var summaryJa: String
  public var summaryEn: String
  public var scopeJa: String
  public var scopeEn: String
  public var instruction: String
  public var failed: UInt32
  public var recovered: UInt32
  public struct Link: Codable, Equatable, Sendable { public var title: String; public var url: String }
  public var links: [Link]
  public var children: [ChatRow]
}

/// The native client's presentation snapshot; hostd owns the full trace.
public struct Conversation: Equatable {
  public var id = ""
  public var rows: [ChatRow] = []
  public var usage = Usage()
  public init() {}
}

public enum AgentEvent: Sendable {
  /// The core snapshot now contains the model accepted for this turn.
  case presentation([ChatRow])
  case modelAccepted
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
  /// Observation found a core-owned interruption; work stays stopped.
  case interrupted
  /// What a helper started by `delegate` does (its own events).
  case helperStarted(String)
  indirect case helper(name: String, event: AgentEvent)
}


/// A UI handle to a durable conversation owned by core. All model calls,
/// tools, persistence and authorization run behind the Agent socket.
public actor Agent {
  public enum Role: String, Sendable { case worker, guide }
  public typealias Approver = @Sendable ([String], [Tsugumi_Agent_AccessGroupT]) async -> Set<String>
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
  public private(set) var resumable = false
  public private(set) var interruptedOperation: Tsugumi_Agent_InterruptedOperationT?
  public private(set) var modelBinding: Tsugumi_Agent_ModelBindingT?
  private var answeredAccess: [String: Bool] = [:]
  private let client = CoreHostClient.shared
  private let request: (String, [String: Any]) async throws -> [String: Any]

  public static let contextFullText = "the conversation no longer fits the model's context"

  public init(id: String, transport: ToolTransport, provider: ProviderConfig, role: Role = .worker) {
    self.request = { try await CoreHostClient.shared.call($0, $1) }
    self.id = id
    self.transport = transport
    self.provider = provider
    self.role = role
    conversation.id = id
  }

  init(id: String, transport: ToolTransport, provider: ProviderConfig,
       request: @escaping (String, [String: Any]) async throws -> [String: Any]) {
    self.id = id
    self.transport = transport
    self.provider = provider
    self.role = .worker
    self.request = request
    conversation.id = id
  }

  public func setProvider(_ config: ProviderConfig) { provider = config }
  public func setApprover(_ handler: Approver?) { approver = handler }

  public func open() async throws {
    var fields: [String: Any] = ["conversation_id": id, "role": role.rawValue]
    if let project = try await transport.project() {
      fields["project_id"] = project.id
    }
    let result = try await self.request("ConversationCreateRequest", fields)
    try adopt(result)
    if !opened { sequence = result["event_seq"] as? UInt64 ?? 0 }
    opened = true
  }

  private func adopt(_ result: [String: Any]) throws {
    guard let value = result["conversation"] as? Tsugumi_Agent_ConversationT, value.id == id else {
      throw CoreHostFailure(code: "invalid_reply", message: "The host returned a different conversation", requestID: nil)
    }
    let snapshot = try ServiceWire.conversation(value)
    conversation = snapshot
    busy = result["busy"] as? Bool ?? false
    resumable = result["resumable"] as? Bool ?? false
    canContinue = result["continuable"] as? Bool ?? false
    interruptedOperation = result["interrupted_operation"] as? Tsugumi_Agent_InterruptedOperationT
    modelBinding = result["model_binding"] as? Tsugumi_Agent_ModelBindingT
  }

  public func refresh() async throws {
    if !opened { return try await open() }
    try adopt(try await self.request("ConversationGetRequest", ["conversation_id": id]))
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
      _ = try await self.request("ConversationCancelRequest", ["conversation_id": id])
      while true {
        let result = try await self.request("ConversationGetRequest", ["conversation_id": id])
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
    let result = try await self.request("ConversationGetRequest", ["conversation_id": id])
    try Task.checkCancellation()
    try adopt(result)
    sequence = result["event_seq"] as? UInt64 ?? sequence
  }

  public func reset() async throws {
    _ = try await self.request("ConversationActionRequest", ["conversation_id": id, "action": "Reset"])
    try await refresh()
  }

  public var hasUnfinishedWork: Bool { busy }

  public func send(prompt: String, title: String, display: String? = nil,
                   selectedCards: [String] = [], selectedDisplays: [String] = [], skillNames: [String] = [], scopedSkill: String? = nil, attachedResources: [String] = [],
                   events: @escaping @Sendable (AgentEvent) -> Void) {
    let turn = UUID().uuidString
    start(events) {
      try await self.open()
      try await self.client.configure(self.provider, conversationID: self.id)
      try await self.beginTurn()
      _ = try await self.request("ConversationSendRequest", ["conversation_id": self.id,
        "turn_id": turn, "text": prompt, "display": display ?? prompt, "selected_cards": selectedCards,
        "selected_displays": selectedDisplays, "skill_names": skillNames, "scoped_skill": scopedSkill as Any, "attached_resources": attachedResources])
    }
  }

  public func retry(events: @escaping @Sendable (AgentEvent) -> Void) { action("Retry", events) }
  public func continueWork(events: @escaping @Sendable (AgentEvent) -> Void) { action("Continue", events) }
  public func compact(events: @escaping @Sendable (AgentEvent) -> Void) { action("Compact", events) }
  public func resume(events: @escaping @Sendable (AgentEvent) -> Void) { action("Resume", events) }
  public func observe(events: @escaping @Sendable (AgentEvent) -> Void) {
    start(events) { try await AgentObservation.read { try await self.open() } }
  }

  private func action(_ name: String, _ events: @escaping @Sendable (AgentEvent) -> Void) {
    start(events) {
      try await self.open()
      try await self.client.configure(self.provider, conversationID: self.id)
      try await self.beginTurn()
      _ = try await self.request("ConversationActionRequest", ["conversation_id": self.id, "action": name])
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
        try await AgentObservation.read { try await self.refresh() }
        try Task.checkCancellation()
        events(.modelAccepted)
        let access = try await AgentObservation.read { try await self.request("AccessListRequest", ["conversation_id": self.id]) }
        try Task.checkCancellation()
        for request in access["requests"] as? [[String: Any]] ?? [] where request["state"] as? String == "pending" {
          guard let requestID = request["id"] as? String else { continue }
          guard let asked = request["asked"] as? [String] else { continue }
          _ = try await answerAccess(requestID, asked: asked, groups: request["groups"] as? [Tsugumi_Agent_AccessGroupT] ?? [])
        }
        while !Task.isCancelled {
          let result = try await AgentObservation.read { try await self.request("EventsRequest", ["conversation_id": self.id, "after_seq": self.sequence]) }
          try Task.checkCancellation()
          var refreshed = false
          for item in result["events"] as? [[String: Any]] ?? [] {
            sequence = max(sequence, item["seq"] as? UInt64 ?? sequence)
            guard let event = item["event"] as? [String: Any] else { continue }
            if event["kind"] as? String == "permission", let request = event["id"] as? String {
              let asked = event["asked"] as? [String] ?? ["project_access Project content"]
              _ = try await answerAccess(request, asked: asked, groups: event["groups"] as? [Tsugumi_Agent_AccessGroupT] ?? [])
            } else if let event = Self.event(event) {
              if case .finished = event { try await AgentObservation.read { try await self.refresh() }; refreshed = true }
              try Task.checkCancellation()
              if case .finished = event, resumable { events(.interrupted) }
              else { events(event) }
            }
          }
          if let rows = result["rows"] as? [ChatRow] { events(.presentation(rows)) }
          sequence = max(sequence, result["next_seq"] as? UInt64 ?? sequence)
          busy = result["busy"] as? Bool ?? false
          if !busy {
            if !refreshed { try await AgentObservation.read { try await self.refresh() } }
            try Task.checkCancellation()
            if resumable { events(.interrupted) }
            break
          }
          try await Task.sleep(for: .milliseconds(80))
        }
      } catch is CancellationError {} catch {
        if !Task.isCancelled {
          // A restarted service owns the interrupted snapshot. Reading it
          // updates the controls without sending an action.
          try? await AgentObservation.read { try await self.refresh() }
          if !Task.isCancelled {
            events(resumable ? .interrupted : .error(String(describing: error)))
          }
        }
      }
    }
  }

  private func answerAccess(_ request: String, asked: [String], groups: [Tsugumi_Agent_AccessGroupT]) async throws -> Bool {
    if let answer = answeredAccess[request] { return answer }
    let access = try await self.request("AccessListRequest", ["conversation_id": id])
    if let state = (access["requests"] as? [[String: Any]])?.first(where: { $0["id"] as? String == request })?["state"] as? String,
       state != "pending" {
      answeredAccess[request] = state == "allowed"
      return state == "allowed"
    }
    guard let approver else { throw CancellationError() }
    let answer = await approver(asked, groups)
    try Task.checkCancellation()
    var allowed = false
    do {
      let result = try await self.request("AccessResolveRequest", ["request_id": request, "allow": !answer.isEmpty, "selected": Array(answer)])
      allowed = result["accepted"] as? Bool ?? false
    } catch let error as CoreHostFailure where error.code == "conflict" {
      // A human in another UI already answered; read the authoritative outcome.
      return try await answerAccess(request, asked: asked, groups: groups)
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
    case "helperStarted": return .helperStarted(text)
    case "helper":
      guard let nested = value["event"] as? [String: Any], let event = event(nested) else { return nil }
      return .helper(name: name, event: event)
    default: return nil
    }
  }

  public static func guideText(_ key: String, _ values: [String: String] = [:]) -> String {
    Shared.fill(Shared.guideTexts[key]!, values)
  }

}

/// Reads can cross a service restart. Stale readiness and a failed reconnect
/// are transient: keep the event cursor and read again until the new service
/// can supply its durable snapshot. Mutating requests never use this retry.
enum AgentObservation {
  static func read<T>(pause: () async throws -> Void = { try await Task.sleep(for: .milliseconds(80)) },
                      _ read: () async throws -> T) async throws -> T {
    while true {
      try Task.checkCancellation()
      do { return try await read() }
      catch RPCError.disconnected { try await pause() }
      catch RPCError.timeout { try await pause() }
      catch let error as CoreHostFailure where error.code == "disconnected" { try await pause() }
    }
  }
}
