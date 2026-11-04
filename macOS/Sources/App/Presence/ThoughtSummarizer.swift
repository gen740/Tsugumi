import Foundation
import TsugumiAgent

/// "思考の過程を描く": what the Agent is thinking, in one short line for the
/// tsugumi's bubble. A small model reads the Agent's recent words and tool
/// calls (at most every few seconds, only while it is on) and writes the
/// line; this costs tokens of its own, counted in `usage`. Off, the bubble
/// shows plain lines built from the scenes, with no extra calls.
@MainActor
final class ThoughtSummarizer {
  let presence: AgentPresence
  private(set) var usage = Usage()
  /// Why the last summary could not be written (nil when it was).
  private(set) var lastError: String?
  private var notes: [String] = []
  private var lastCall = Date.distantPast
  private var task: Task<Void, Never>?

  static let interval = 4.0
  static let defaultsKey = "agent.drawThoughts"

  init(presence: AgentPresence) { self.presence = presence }

  /// Switched in the chat; asks once before it is first turned on (ChatPanel).
  static var enabled: Bool { UserDefaults.standard.bool(forKey: defaultsKey) }

  func turnStarted() { notes.removeAll() }

  func note(text: String) {
    if let last = notes.last, last.hasPrefix("assistant: ") {
      notes[notes.count - 1] += text
    } else {
      notes.append("assistant: " + text)
    }
    schedule()
  }

  func note(tool: String, arguments: String) {
    notes.append("tool \(tool): " + arguments.prefix(300))
    schedule()
  }

  func note(tool: String, ok: Bool, summary: String) {
    notes.append("result of \(tool) (\(ok ? "ok" : "error")): " + summary.prefix(200))
    schedule()
  }

  private func schedule() {
    guard Self.enabled, task == nil, let config = Self.config() else { return }
    let wait = max(0, Self.interval - Date().timeIntervalSince(lastCall))
    task = Task { [weak self] in
      try? await Task.sleep(for: .seconds(wait))
      guard let self else { return }
      let input = String(self.notes.joined(separator: "\n").suffix(2400))
      self.notes.removeAll()
      self.lastCall = Date()
      do {
        let reply = try await Self.summarize(input, config: config, usage: { self.usage.inputTokens += $0.inputTokens; self.usage.outputTokens += $0.outputTokens })
        if let line = Self.clean(reply) { self.presence.think(line) }
        self.lastError = nil
      } catch {
        // The bubble keeps its plain lines; the reason is kept for checks.
        self.lastError = "\(error)"
      }
      self.task = nil
      if !self.notes.isEmpty { self.schedule() }
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
      c.maxTokens = 120
      return c
    }
    switch c.api {
    case .anthropicMessages: c.model = d.string(forKey: "agent.thoughtModel").flatMap { $0.isEmpty ? nil : $0 } ?? "claude-haiku-5-5"
    case .openAIResponses: c.model = d.string(forKey: "agent.thoughtModel.openai").flatMap { $0.isEmpty ? nil : $0 } ?? c.model
    }
    c.maxTokens = 120
    return c
  }

  /// The bubble speaks in the app's language and in the voice of its plain
  /// lines (「load を読んでいます」), so a summary does not change the tone.
  static var system: String { system(english: Bundle.main.preferredLocalizations.first?.hasPrefix("en") == true) }

  nonisolated static func system(english: Bool) -> String {
    let voice = english
      ? "one line of English, at most 60 characters, in the present progressive like the app's other lines " +
        "(\"Reading load\", \"Running from start\"), for example \"Widening the band edges so green stands out\""
      : "one line of Japanese, at most 40 characters, in the polite form ending in 「〜しています」 like the app's " +
        "other lines (「load を読んでいます」「start から実行しています」), for example 「帯域の境界を広げて、緑を見やすくしています」"
    return """
    You watch an AI agent (and the helpers it may have started) build a data-processing workflow of Cards in \
    an app and say what it is doing right now, for a small speech bubble beside it on the canvas. Reply with \
    \(voice). Say what it does and why when \
    that is clear, naming Cards as the notes do but never the tools (say "waiting for the run to end", not \
    run_status); no first person, no greetings, no quotes, no Markdown, never invent facts that are not in \
    the notes.
    """
  }

  static func summarize(_ notes: String, config: ProviderConfig, usage: @escaping (Usage) -> Void) async throws -> String {
    let provider = MessagesProvider(config: config)
    var text = ""
    for try await e in try provider.stream(system: system, messages: [ChatMessage(role: "user", content: [.text(notes)])], tools: []) {
      switch e {
      case .text(let t): text += t
      case .stop(_, let u): usage(u)
      default: break
      }
    }
    return text
  }

  /// One line, at most 60 characters, without quotes or Markdown.
  nonisolated static func clean(_ text: String) -> String? {
    let line = text.split(whereSeparator: \.isNewline).first.map(String.init)?
      .trimmingCharacters(in: CharacterSet(charactersIn: " \t「」\"'`*#-")) ?? ""
    return line.isEmpty ? nil : String(line.prefix(60))
  }
}
