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
  case presentation([ChatRow])
  case presentationPatch(count: Int, rows: [(Int, ChatRow)])
  /// The core snapshot now contains the model accepted for this turn.
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
  case error(String, code: String? = nil)
  case cancelled
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
  public typealias Approver = @Sendable ([String], String?) async -> Set<String>
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
  /// A run the Agent started brings the conversation back when it ends.
  public private(set) var runWake = true
  public private(set) var interruptedOperation: Tsugumi_Agent_InterruptedOperationT?
  public private(set) var modelBinding: Tsugumi_Agent_ModelBindingT?
  private var answeredDecisions: [String: Bool] = [:]
  private let client = CoreHostClient.shared
  private let request: (String, [String: Any]) async throws -> NativeObject

  public init(id: String, transport: ToolTransport, provider: ProviderConfig, role: Role = .worker) {
    self.request = { try await CoreHostClient.shared.agentReply($0, $1) }
    self.id = id
    self.transport = transport
    self.provider = provider
    self.role = role
    conversation.id = id
  }

  init(id: String, transport: ToolTransport, provider: ProviderConfig,
       request: @escaping (String, [String: Any]) async throws -> NativeObject) {
    self.id = id
    self.transport = transport
    self.provider = provider
    self.role = .worker
    self.request = request
    conversation.id = id
  }

  public func setProvider(_ config: ProviderConfig) { provider = config }
  public func setApprover(_ handler: Approver?) { approver = handler }

  private func reply<T: NativeObject>(_ method: String, _ fields: [String: Any]) async throws -> T {
    guard let value = try await request(method, fields) as? T else {
      throw RPCError.unexpectedReply(method)
    }
    return value
  }

  private func view(_ method: String, _ fields: [String: Any]) async throws -> [String: Any] {
    try await ServiceWire.replyView(request(method, fields))
  }

  public func open() async throws {
    var fields: [String: Any] = ["conversation_id": id, "role": role.rawValue]
    if let project = try await transport.project() {
      fields["project_id"] = project.id
    }
    let result: Tsugumi_Agent_ConversationResultT = try await reply("ConversationCreateRequest", fields)
    try adopt(result)
    if !opened { sequence = result.eventSeq }
    opened = true
  }

  private func adopt(_ result: Tsugumi_Agent_ConversationResultT) throws {
    conversation = try ServiceWire.conversation(result.conversation)
    busy = result.busy
    resumable = result.resumable
    canContinue = result.continuable
    runWake = result.runWake
    interruptedOperation = result.interruptedOperation
    modelBinding = result.modelBinding
  }

  public func refresh() async throws {
    if !opened { return try await open() }
    try adopt(try await reply("ConversationGetRequest", ["conversation_id": id]))
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
        let result: Tsugumi_Agent_ConversationResultT = try await self.reply("ConversationGetRequest", ["conversation_id": id])
        try adopt(result)
        if !busy {
          sequence = result.eventSeq
          break
        }
        try await Task.sleep(for: .milliseconds(20))
      }
    }
    transition = stopping
    _ = try? await stopping.value
  }

  private func beginTurn() async throws {
    let result: Tsugumi_Agent_ConversationResultT = try await self.reply("ConversationGetRequest", ["conversation_id": id])
    try Task.checkCancellation()
    try adopt(result)
    sequence = result.eventSeq
  }

  public func reset() async throws {
    _ = try await self.request("ConversationActionRequest", ["conversation_id": id, "action": "Reset"])
    try await refresh()
  }

  public var hasUnfinishedWork: Bool { busy }

  public func setRunWake(_ on: Bool) async throws {
    try await open()
    _ = try await request("ConversationRunWakeRequest", ["conversation_id": id, "run_wake": on])
    runWake = on
  }

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
        events(.presentation(conversation.rows))
        events(.modelAccepted)
        var cursor: Int64 = 0
        repeat {
          let pending = try await AgentObservation.read {
            try await self.view("DecisionListRequest", ["conversation_id": self.id, "pending_only": true, "cursor": cursor])
          }
          try Task.checkCancellation()
          for request in pending["decisions"] as? [[String: Any]] ?? [] where request["state"] as? String == "pending" {
            guard let requestID = request["id"] as? String else { continue }
            _ = try await answerDecision(requestID)
          }
          cursor = pending["next_cursor"] as? Int64 ?? 0
        } while cursor != 0
        while !Task.isCancelled {
          let result: Tsugumi_Agent_EventsPageT = try await AgentObservation.read { try await self.reply("EventsRequest", ["conversation_id": self.id, "after_seq": self.sequence]) }
          try Task.checkCancellation()
          var refreshed = false
          for item in result.events.compactMap({ $0 }) {
            let event = try ServiceWire.replyView(item.event)
            if event["kind"] as? String == "decision", let request = event["id"] as? String {
              _ = try await answerDecision(request)
            } else if let event = Self.event(event) {
              if case .finished = event { try await AgentObservation.read { try await self.refresh() }; refreshed = true }
              try Task.checkCancellation()
              if case .finished = event, resumable { events(.interrupted) }
              else { events(event) }
            }
          }
          if let update = result.presentation {
            let rows = update.rows.compactMap { entry -> (Int, ChatRow)? in
              guard let entry else { return nil }
              return (Int(entry.index), ServiceWire.chatRow(entry.row))
            }
            events(update.reset ? .presentation(rows.map { $0.1 })
                                : .presentationPatch(count: Int(update.rowCount), rows: rows))
          }
          sequence = result.nextSeq
          busy = result.busy
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
            events(resumable ? .interrupted : .error(String(describing: error), code: (error as? CoreHostFailure)?.code))
          }
        }
      }
    }
  }

  private func answerDecision(_ request: String) async throws -> Bool {
    if let answer = answeredDecisions[request] { return answer }
    let listed = try await self.view("DecisionListRequest", ["conversation_id": id, "request_id": request])
    guard let entry = (listed["decisions"] as? [[String: Any]])?.first,
          let state = entry["state"] as? String else { return false }
    if state == "unavailable" { return false }
    if state != "pending" {
      answeredDecisions[request] = state == "allowed"
      return state == "allowed"
    }
    guard let approver else { throw CancellationError() }
    let answer = await approver(entry["asked"] as? [String] ?? [], entry["call_id"] as? String)
    try Task.checkCancellation()
    var allowed = false
    do {
      let result = try await self.view("DecisionResolveRequest", ["request_id": request, "allow": !answer.isEmpty])
      allowed = result["accepted"] as? Bool ?? false
    } catch let error as CoreHostFailure where error.code == "conflict" {
      // A human in another UI already answered; read the authoritative outcome.
      return try await answerDecision(request)
    }
    answeredDecisions[request] = allowed
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
    case "error": return .error(value["message"] as? String ?? text, code: value["code"] as? String)
    case "cancelled": return .cancelled
    case "finished": return .finished
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
      catch let error as CoreHostFailure where error.code == "disconnected" { try await pause() }
    }
  }
}
