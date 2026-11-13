import Foundation
import TsugumiRPC

/// A page a web search found or a reply cites.
public struct WebSource: Codable, Equatable, Sendable {
  public var url: String
  public var title: String?
  public init(url: String, title: String? = nil) {
    self.url = url
    self.title = title
  }
}

/// A search the provider ran: what was asked and what came back.
public struct WebSearch: Codable, Equatable, Sendable {
  public var id: String
  public var query: String
  public var sources: [WebSource] = []
  public var error: String?

  /// What it found, for the chat's row and the tool record.
  public var summary: String {
    if let error { return "failed: \(error)" }
    let lines = sources.map { s in s.title.map { "\($0) — \(s.url)" } ?? s.url }
    return lines.isEmpty ? "no results" : lines.joined(separator: "\n")
  }

  /// What the tool was called with, as the chat shows a tool's arguments.
  public var arguments: String {
    String(decoding: (try? JSONSerialization.data(withJSONObject: ["query": query], options: [.sortedKeys])) ?? Data(), as: UTF8.self)
  }

  /// The cited sources as Markdown links under a reply. They come from the
  /// web: only http(s) is linked, and a title cannot break out of its link.
  static func sourcesText(_ sources: [WebSource]) -> String {
    var seen = Set<String>()
    var lines: [String] = []
    for s in sources {
      let lower = s.url.lowercased()
      guard lower.hasPrefix("https://") || lower.hasPrefix("http://"), seen.insert(s.url).inserted else { continue }
      var url = ""
      for c in s.url {
        switch c {
        case "(": url += "%28"
        case ")": url += "%29"
        case "<": url += "%3C"
        case ">": url += "%3E"
        case _ where c.isWhitespace: url += "%20"
        default: url.append(c)
        }
      }
      let cleaned = (s.title ?? s.url).split(whereSeparator: { $0 == "[" || $0 == "]" || $0.isNewline }).joined(separator: " ")
        .trimmingCharacters(in: .whitespaces)
      lines.append("- [\(cleaned.isEmpty ? s.url : cleaned)](\(url))")
    }
    // The app's language (AppPreferences writes it as the app's AppleLanguages).
    let heading = Shared.data.webSearch.sources[Bundle.main.preferredLocalizations.first == "en" ? "en" : "ja"]!
    return lines.isEmpty ? "" : "\n\n\(heading):\n\(lines.joined(separator: "\n"))\n"
  }
}

/// One content block of a conversation snapshot returned by core.
public enum ContentBlock: Codable, Equatable {
  case text(String)
  case toolUse(id: String, name: String, input: String)  // input: JSON text
  case toolResult(toolUseId: String, content: String, isError: Bool, images: [ToolImage] = [])
  case webSearch(WebSearch)

  enum CodingKeys: String, CodingKey { case type, text, id, name, input, tool_use_id, content, is_error, images }

  public init(from d: Decoder) throws {
    let c = try d.container(keyedBy: CodingKeys.self)
    switch try c.decode(String.self, forKey: .type) {
    case "text": self = .text(try c.decode(String.self, forKey: .text))
    case "tool_use": self = .toolUse(id: try c.decode(String.self, forKey: .id), name: try c.decode(String.self, forKey: .name),
                                     input: try c.decode(String.self, forKey: .input))
    case "web_search": self = .webSearch(try WebSearch(from: d))
    default: self = .toolResult(toolUseId: try c.decode(String.self, forKey: .tool_use_id),
                                content: try c.decode(String.self, forKey: .content),
                                isError: try c.decode(Bool.self, forKey: .is_error),
                                images: try c.decodeIfPresent([ToolImage].self, forKey: .images) ?? [])
    }
  }

  public func encode(to e: Encoder) throws {
    var c = e.container(keyedBy: CodingKeys.self)
    switch self {
    case .text(let t):
      try c.encode("text", forKey: .type)
      try c.encode(t, forKey: .text)
    case .toolUse(let id, let name, let input):
      try c.encode("tool_use", forKey: .type)
      try c.encode(id, forKey: .id)
      try c.encode(name, forKey: .name)
      try c.encode(input, forKey: .input)
    case .toolResult(let id, let content, let isError, let images):
      try c.encode("tool_result", forKey: .type)
      try c.encode(id, forKey: .tool_use_id)
      try c.encode(content, forKey: .content)
      try c.encode(isError, forKey: .is_error)
      if !images.isEmpty { try c.encode(images, forKey: .images) }
    case .webSearch(let s):
      try c.encode("web_search", forKey: .type)
      try s.encode(to: e)
    }
  }


}

public struct ChatMessage: Codable, Equatable {
  public var role: String  // "user" | "assistant"
  public var content: [ContentBlock]
  /// What the chat shows for a request the app wrote for the model (the
  /// model gets `content`; this is never sent).
  public var display: String?
  public init(role: String, content: [ContentBlock], display: String? = nil) {
    self.role = role
    self.content = content
    self.display = display
  }
}

public struct Usage: Codable, Equatable, Sendable {
  /// Context of the last request (uncached, cache read and cache written).
  public var inputTokens = 0
  public var outputTokens = 0
  /// Input served from the provider's prompt cache, and written to it.
  public var cacheReadTokens = 0
  public var cacheWriteTokens = 0
  public init() {}

  enum CodingKeys: String, CodingKey { case inputTokens, outputTokens, cacheReadTokens, cacheWriteTokens }
  public init(from d: Decoder) throws {
    // Conversations saved before the cache counts are read as they are.
    let c = try d.container(keyedBy: CodingKeys.self)
    inputTokens = try c.decodeIfPresent(Int.self, forKey: .inputTokens) ?? 0
    outputTokens = try c.decodeIfPresent(Int.self, forKey: .outputTokens) ?? 0
    cacheReadTokens = try c.decodeIfPresent(Int.self, forKey: .cacheReadTokens) ?? 0
    cacheWriteTokens = try c.decodeIfPresent(Int.self, forKey: .cacheWriteTokens) ?? 0
  }
}

public struct ProviderConfig: Codable, Equatable, Sendable {
  /// Wire protocol of the endpoint.
  public enum API: String, Codable, Sendable { case anthropicMessages, openAIResponses }
  /// How requests are authorized: none (the local fixture), an API key held by agentd, or the user's ChatGPT plan (Sign in with ChatGPT).
  public enum Auth: String, Codable, Sendable { case none, apiKey, chatGPT }

  public var name: String
  public var model: String
  public var auth: Auth
  public var api: API = .anthropicMessages
  /// The chat's choices in the model menu, already checked against the model
  /// (`choosing`). None for the requests that are not the Agent's work.
  public var effort: String?
  public var fast = false
  public var usesChoices = false

  public init(name: String, model: String, auth: Auth, api: API = .anthropicMessages) {
    self.name = name
    self.model = model
    self.auth = auth
    self.api = api
  }

  /// A real provider (not the local fixture).
  public var isRemote: Bool { auth != .none }

  public static let anthropic = ProviderConfig(name: "Anthropic",
                                               model: "claude-sonnet-5-5", auth: .apiKey)
  /// OpenAI with an API key (Responses API).
  public static let openAI = ProviderConfig(name: "OpenAI",
                                            model: "gpt-6.1-sol", auth: .apiKey, api: .openAIResponses)
  /// OpenAI with the user's ChatGPT plan (Sign in with ChatGPT, open-source flow).
  public static let chatGPT = ProviderConfig(name: "ChatGPT",
                                             model: "gpt-6.1-sol", auth: .chatGPT, api: .openAIResponses)
  public static let fixture = ProviderConfig(name: "Local fixture", model: "fixture", auth: .none)

}

/// Secret input and status adapters for agentd's owner-only credential store.
public enum ProviderCredentials {
  private static let lock = NSLock()
  nonisolated(unsafe) private static var configured: [String: Bool] = [:]
  private static func id(_ name: String) -> String { name == "OpenAI" ? "openai" : "anthropic" }
  public static func exists(account: String) -> Bool { lock.lock(); defer { lock.unlock() }; return configured[id(account)] ?? false }
  private static func remember(_ id: String, _ value: Bool) { lock.lock(); defer { lock.unlock() }; configured[id] = value }
  public static func refresh() async {
    for id in ["anthropic", "openai"] {
      let result = try? await CoreAgentClient.shared.call("ProviderStatusRequest", ["profile": "desktop", "id": id])
      if let v = result?["value"] as? [String: Any] { remember(id, v["configured"] as? Bool ?? false) }
    }
  }
  public static func save(_ secret: String, account: String) async throws {
    let p = Tsugumi_Service_ProviderInputT(); p.apiKey = secret
    _ = try await CoreAgentClient.shared.call("ProviderSetRequest", ["profile": "desktop", "id": id(account), "provider": p])
    remember(id(account), true)
  }
  public static func delete(account: String) async throws {
    _ = try await CoreAgentClient.shared.call("ProviderSetRequest", ["profile": "desktop", "id": id(account), "remove": true])
    remember(id(account), false)
  }
}

/// Model discovery and credential lookup are owned by agentd.
public struct MessagesProvider: Sendable {
  public let config: ProviderConfig
  public init(config: ProviderConfig) { self.config = config }
  public func models() async throws -> [String] { try await modelChoices().map(\.id) }
  public func modelChoices() async throws -> [(id: String, name: String)] {
    try await CoreAgentClient.shared.models(config)
  }
}
