import Foundation
import Security
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

public enum Keychain {
  static let service = "app.tsugumi.agent"

  /// Operations under way that may show the access dialog, and the lock that
  /// keeps a read without asking from turning their dialog off.
  private static let gate = NSLock()
  nonisolated(unsafe) private static var mayAsk = 0

  private static func askingAllowed<T>(_ body: () -> T) -> T {
    gate.lock()
    mayAsk += 1
    gate.unlock()
    defer {
      gate.lock()
      mayAsk -= 1
      gate.unlock()
    }
    return body()
  }

  /// Removes the stored secret of a provider (no error when there is none).
  public static func delete(account: String) {
    let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                            kSecAttrAccount as String: account]
    askingAllowed { _ = SecItemDelete(q as CFDictionary) }
  }

  /// Saves in place when the item exists (a reader never sees it missing).
  public static func save(_ secret: String, account: String) throws {
    let base: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                               kSecAttrAccount as String: account]
    let u = askingAllowed { SecItemUpdate(base as CFDictionary, [kSecValueData as String: Data(secret.utf8)] as CFDictionary) }
    if u == errSecSuccess { return }
    guard u == errSecItemNotFound else { throw AgentError.provider("Keychain error \(u)") }
    var add = base
    add[kSecValueData as String] = Data(secret.utf8)
    add[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
    let s = askingAllowed { SecItemAdd(add as CFDictionary, nil) }
    guard s == errSecSuccess else { throw AgentError.provider("Keychain error \(s)") }
  }

  /// Whether a provider's secret is stored, without reading it (no access prompt).
  public static func exists(account: String) -> Bool {
    let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                            kSecAttrAccount as String: account, kSecReturnAttributes as String: true]
    var out: CFTypeRef?
    return SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess
  }

  public static func load(account: String) -> String? {
    let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                            kSecAttrAccount as String: account, kSecReturnData as String: true]
    var out: CFTypeRef?
    guard askingAllowed({ SecItemCopyMatching(q as CFDictionary, &out) }) == errSecSuccess, let d = out as? Data else { return nil }
    return String(data: d, encoding: .utf8)
  }

  /// The secret if this program may read it now; never shows the access
  /// dialog (nil: no such item, or it would have to ask). For reads nobody
  /// asked for, such as listing models when a menu opens.
  ///
  /// The switch is process-wide, so it is off only for this read and never
  /// while another operation may be asking. It is deprecated, but nothing
  /// replaces it: a login keychain item still shows its dialog with
  /// kSecUseAuthenticationUIFail.
  public static func loadWithoutAsking(account: String) -> String? {
    let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                            kSecAttrAccount as String: account, kSecReturnData as String: true]
    gate.lock()
    defer { gate.unlock() }
    guard mayAsk == 0 else { return nil }
    SecKeychainSetUserInteractionAllowed(false)
    defer { SecKeychainSetUserInteractionAllowed(true) }
    var out: CFTypeRef?
    guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
    return String(data: d, encoding: .utf8)
  }
}

/// Platform secret storage and explicit, memory-only provisioning. Settings,
/// provider interpretation and OAuth remain in agentd.
public enum ProviderCredentials {
  public static func exists(account: String) -> Bool { Keychain.exists(account: account) }
  public static func save(_ secret: String, account: String) async throws {
    try Keychain.save(secret, account: account)
    try await CoreAgentClient.shared.provisionKey(secret, id: account == "OpenAI" ? "openai" : "anthropic")
  }
  public static func delete(account: String) async throws {
    Keychain.delete(account: account)
    try await CoreAgentClient.shared.provisionKey(nil, id: account == "OpenAI" ? "openai" : "anthropic")
  }
}

/// Model discovery belongs to agentd; the platform controls Keychain access.
public struct MessagesProvider: Sendable {
  public let config: ProviderConfig
  public init(config: ProviderConfig) { self.config = config }
  public func models() async throws -> [String] { try await modelChoices().map(\.id) }
  public func modelChoices(asking: Bool = true) async throws -> [(id: String, name: String)] {
    try await CoreAgentClient.shared.models(config, asking: asking)
  }
}
