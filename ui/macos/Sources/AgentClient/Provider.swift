import Foundation
import Security
import TsugumiKeychainUI
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

// This boundary only denies access; no test setting can enable the real store.
// Injected runners implement a fake without executing the supplied OS operation.
func runSystemKeychain(_ operation: String, _ body: () -> OSStatus) -> OSStatus {
  let environment = ProcessInfo.processInfo.environment
  guard environment["TSUGUMI_TEST_RUN_ROOT"] == nil,
        environment["TSUGUMI_TEST_NO_SECRET_STORE"] != "1" else { return errSecNotAvailable }
  return body()
}

final class KeychainStore {
  typealias Run = (String, () -> OSStatus) -> OSStatus
  private let execute: Run
  init(run: @escaping Run = { _, body in body() }) { self.execute = run }
  private func run(_ operation: String, _ body: () -> OSStatus) -> OSStatus {
    // Even an injected runner that invokes its body cannot bypass isolation.
    execute(operation) { runSystemKeychain(operation, body) }
  }
  static let service = "app.tsugumi.agent"

  /// Operations under way that may show the access dialog, and the lock that
  /// keeps a read without asking from turning their dialog off.
  private let gate = NSLock()
  private var mayAsk = 0

  private func askingAllowed<T>(_ body: () -> T) -> T {
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
  func delete(account: String) {
    let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: Self.service,
                            kSecAttrAccount as String: account]
    askingAllowed { _ = run("SecItemDelete") { SecItemDelete(q as CFDictionary) } }
  }

  /// Saves in place when the item exists (a reader never sees it missing).
  func save(_ secret: String, account: String) throws {
    let base: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: Self.service,
                               kSecAttrAccount as String: account]
    let u = askingAllowed { run("SecItemUpdate") { SecItemUpdate(base as CFDictionary, [kSecValueData as String: Data(secret.utf8)] as CFDictionary) } }
    if u == errSecSuccess { return }
    guard u == errSecItemNotFound else { throw AgentError.provider("Keychain error \(u)") }
    var add = base
    add[kSecValueData as String] = Data(secret.utf8)
    add[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
    let s = askingAllowed { run("SecItemAdd") { SecItemAdd(add as CFDictionary, nil) } }
    guard s == errSecSuccess else { throw AgentError.provider("Keychain error \(s)") }
  }

  /// Whether a provider's secret is stored, without reading it (no access prompt).
  func exists(account: String) -> Bool {
    let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: Self.service,
                            kSecAttrAccount as String: account, kSecReturnAttributes as String: true]
    var out: CFTypeRef?
    return run("SecItemCopyMatching(attributes)") { SecItemCopyMatching(q as CFDictionary, &out) } == errSecSuccess
  }

  func load(account: String) -> String? {
    let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: Self.service,
                            kSecAttrAccount as String: account, kSecReturnData as String: true]
    var out: CFTypeRef?
    guard askingAllowed({ run("SecItemCopyMatching(data)") { SecItemCopyMatching(q as CFDictionary, &out) } }) == errSecSuccess, let d = out as? Data else { return nil }
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
  func loadWithoutAsking(account: String) -> String? {
    let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: Self.service,
                            kSecAttrAccount as String: account, kSecReturnData as String: true]
    gate.lock()
    defer { gate.unlock() }
    guard mayAsk == 0 else { return nil }
    var out: CFTypeRef?
    let status = run("SecItemCopyMatching(without asking)") {
      TsugumiSetKeychainInteractionAllowed(false)
      defer { TsugumiSetKeychainInteractionAllowed(true) }
      return SecItemCopyMatching(q as CFDictionary, &out)
    }
    guard status == errSecSuccess, let d = out as? Data else { return nil }
    return String(data: d, encoding: .utf8)
  }
}

public enum Keychain {
  private static let store = KeychainStore()
  public static func delete(account: String) { store.delete(account: account) }
  public static func save(_ secret: String, account: String) throws { try store.save(secret, account: account) }
  public static func exists(account: String) -> Bool { store.exists(account: account) }
  public static func load(account: String) -> String? { store.load(account: account) }
  public static func loadWithoutAsking(account: String) -> String? { store.loadWithoutAsking(account: account) }
}

/// Platform secret storage and explicit, memory-only provisioning. Settings,
/// provider interpretation and OAuth remain in hostd.
public enum ProviderCredentials {
  public static func exists(account: String) -> Bool { ModelCatalogSnapshot.shared.usable(account == "OpenAI" ? "openai" : "anthropic") }
  public static func save(_ secret: String, account: String) async throws {
    try Keychain.save(secret, account: account)
    try await CoreHostClient.shared.provisionKey(secret, id: account == "OpenAI" ? "openai" : "anthropic")
  }
  public static func delete(account: String) async throws {
    Keychain.delete(account: account)
    try await CoreHostClient.shared.provisionKey(nil, id: account == "OpenAI" ? "openai" : "anthropic")
  }
}

/// Model discovery belongs to hostd; the platform controls Keychain access.
public struct MessagesProvider: Sendable {
  public let config: ProviderConfig
  public init(config: ProviderConfig) { self.config = config }
  public func models() async throws -> [String] { try await modelChoices().map(\.id) }
  public func modelChoices(asking: Bool = true) async throws -> [(id: String, name: String)] {
    try await CoreHostClient.shared.models(config, asking: asking)
  }
}
