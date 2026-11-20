import Foundation
import TsugumiRPC

public struct Usage: Codable, Equatable, Sendable {
  /// Context of the last request (uncached, cache read and cache written).
  public var inputTokens = 0
  public var outputTokens = 0
  /// Input served from the provider's prompt cache, and written to it.
  public var cacheReadTokens = 0
  public var cacheWriteTokens = 0
  public init() {}


}

public struct ProviderConfig: Codable, Equatable, Sendable {
  /// Wire protocol of the endpoint.
  public enum API: String, Codable, Sendable { case anthropicMessages, openAIResponses }
  /// How requests are authorized: none (the local fixture), an API key held by hostd, or the user's ChatGPT plan (Sign in with ChatGPT).
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
  public var purpose: String?

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

/// API keys are stored by hostd (on every platform); the app saves and
/// removes them there and never keeps a copy.
public enum ProviderCredentials {
  public static func exists(account: String) -> Bool { ModelCatalogSnapshot.shared.usable(account == "OpenAI" ? "openai" : "anthropic") }
  public static func save(_ secret: String, account: String) async throws {
    try await CoreHostClient.shared.saveKey(secret, id: account == "OpenAI" ? "openai" : "anthropic")
  }
  public static func delete(account: String) async throws {
    try await CoreHostClient.shared.saveKey(nil, id: account == "OpenAI" ? "openai" : "anthropic")
  }
}

/// Model discovery belongs to hostd.
public struct MessagesProvider: Sendable {
  public let config: ProviderConfig
  public init(config: ProviderConfig) { self.config = config }
  public func models() async throws -> [String] { try await modelChoices().map(\.id) }
  public func modelChoices() async throws -> [(id: String, name: String)] {
    try await CoreHostClient.shared.models(config)
  }
}
