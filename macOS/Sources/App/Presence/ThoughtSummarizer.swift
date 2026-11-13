import Foundation
import TsugumiAgentClient

/// "Draw thoughts": what the Agent is thinking, in one short line for the
/// tsugumi's bubble. A small model reads the Agent's recent words and tool
/// calls (at most every few seconds, only while it is on) and writes the
/// line; this costs tokens of its own, counted in `usage`. Off, the bubble
/// shows plain lines built from the scenes, with no extra calls.
@MainActor
final class ThoughtSummarizer {
  let presence: AgentPresence
  var conversationID: String?
  private(set) var usage = Usage()
  /// Why the last summary could not be written (nil when it was).
  private(set) var lastError: String?
  private var task: Task<Void, Never>?

  static let defaultsKey = "agent.drawThoughts"

  init(presence: AgentPresence) { self.presence = presence }

  /// Switched in the chat; asks once before it is first turned on (ChatPanel).
  static var enabled: Bool { UserDefaults.standard.bool(forKey: defaultsKey) }

  /// Polling refreshes this window; eligibility and throttling belong to agentd.
  func turnStarted() {
    task?.cancel()
    let id = conversationID
    task = Task { [weak self] in
      while !Task.isCancelled {
        guard let self, self.presence.working, self.conversationID == id else { return }
        if Self.enabled, let config = Self.config(), let id {
          do {
            let reply = try await Self.summarize(conversationID: id, config: config, usage: {
              self.usage.inputTokens += $0.inputTokens
              self.usage.outputTokens += $0.outputTokens
            })
            guard !Task.isCancelled, self.conversationID == id else { return }
            if Self.enabled, let line = Self.clean(reply) { self.presence.think(line) }
            self.lastError = nil
          } catch {
            self.lastError = "\(error)"
          }
        }
        try? await Task.sleep(for: .seconds(1))
      }
    }
  }

  /// The model for the summaries: a small one of the Agent's provider (the
  /// local fixture has none: plain lines only; on the ChatGPT plan, the
  /// small speech model).
  static func config() -> ProviderConfig? {
    var c = AgentSettings.current()
    guard c.auth != .none else { return nil }
    let d = UserDefaults.standard
    if c.auth == .chatGPT {
      c.model = AgentSettings.chatGPTSpeechModel
      return c
    }
    switch c.api {
    case .anthropicMessages: c.model = d.string(forKey: "agent.thoughtModel").flatMap { $0.isEmpty ? nil : $0 } ?? "claude-haiku-5-5"
    case .openAIResponses: c.model = d.string(forKey: "agent.thoughtModel.openai").flatMap { $0.isEmpty ? nil : $0 } ?? c.model
    }
    return c
  }

  static func summarize(conversationID: String, config: ProviderConfig, usage: @escaping (Usage) -> Void) async throws -> String {
    let client = CoreAgentClient.shared
    var config = config
    if config.auth == .chatGPT {
      _ = try? await client.storedModels("chatgpt")
      config.model = AgentSettings.chatGPTSpeechModel
    }
    let value = try await client.call("ConversationSummarizeRequest", ["conversation_id": conversationID,
      "provider": try await client.provider(config)])
    if let counts = value["usage"], let data = try? JSONSerialization.data(withJSONObject: counts),
       let measured = try? JSONDecoder().decode(Usage.self, from: data) { usage(measured) }
    return value["text"] as? String ?? ""
  }

  /// One line, at most 60 characters, without quotes or Markdown.
  nonisolated static func clean(_ text: String) -> String? {
    let line = text.split(whereSeparator: \.isNewline).first.map(String.init)?
      .trimmingCharacters(in: CharacterSet(charactersIn: " \t「」\"'`*#-")) ?? ""
    return line.isEmpty ? nil : String(line.prefix(60))
  }
}
