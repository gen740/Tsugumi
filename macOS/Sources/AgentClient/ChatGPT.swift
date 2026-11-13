import Foundation

/// Where the sign-in happens (OpenAI, or the local fixture in tests).
public struct ChatGPTEndpoints: Codable, Equatable, Sendable {
  public var issuer: String
  /// The resource the tokens are for (the Responses API).
  public var resource: String

  public init(issuer: String, resource: String) {
    self.issuer = issuer
    self.resource = resource
  }

  public static let openAI = ChatGPTEndpoints(issuer: "https://auth.openai.com", resource: "https://api.openai.com/v1")
}

/// What the settings show of a registration (never a token).
public struct ChatGPTAccountInfo: Equatable, Sendable, Identifiable {
  public var id: String { clientId }
  public var clientId: String
  public var email: String?
  public var signedIn: Bool
  public var planEnabled: Bool
  public var active: Bool
}

public enum ChatGPTError: Error, CustomStringConvertible, Equatable {
  case failed(String)
  public var description: String {
    switch self { case .failed(let message): return "ChatGPT: \(message)" }
  }

  /// The chat shows "Manage usage" with this error.
  public static let usageLimitText = "ChatGPT usage limit reached: review your plan or this app's limit in ChatGPT settings"
}

/// Browser opening and status polling; all OAuth and credential state is in agentd.
public actor ChatGPTAuth {
  public static let usageURL = URL(string: "https://chatgpt.com/settings/usage")!
  private let endpoints: ChatGPTEndpoints
  private let profile: String
  public static func shared(endpoints: ChatGPTEndpoints = .openAI, store: String = "ChatGPT") -> ChatGPTAuth {
    ChatGPTAuth(endpoints: endpoints, profile: store == "ChatGPT" ? "desktop" : store)
  }
  private init(endpoints: ChatGPTEndpoints, profile: String) { self.endpoints = endpoints; self.profile = profile }
  private func request(_ action: String? = nil, client: String? = nil, attempt: String? = nil) async throws -> [String: Any] {
    var fields: [String: Any] = ["profile": profile]
    if let action { fields["action"] = action }
    if let client { fields["client_id"] = client }
    if let attempt { fields["attempt_id"] = attempt }
    let reply = try await CoreAgentClient.shared.call(action == nil ? "SignInStatusRequest" : "SignInActionRequest", fields)
    return reply["value"] as? [String: Any] ?? [:]
  }
  private func infos(_ value: [String: Any]) -> [ChatGPTAccountInfo] {
    (value["accounts"] as? [[String: Any]] ?? []).map { a in
      ChatGPTAccountInfo(clientId: a["clientId"] as? String ?? "", email: a["email"] as? String,
        signedIn: a["signedIn"] as? Bool ?? false, planEnabled: a["planEnabled"] as? Bool ?? false, active: a["active"] as? Bool ?? false)
    }
  }
  public func accounts() async -> [ChatGPTAccountInfo] { infos((try? await request()) ?? [:]) }
  public func accountsWithoutAsking() async -> [ChatGPTAccountInfo]? { guard let v = try? await request() else { return nil }; return infos(v) }
  public func select(clientId: String) async throws { _ = try await request("select", client: clientId) }
  public func markWelcomed(clientId: String) async throws { _ = try await request("welcomed", client: clientId) }
  public func signOut(clientId: String) async throws -> Bool { try await request("signout", client: clientId)["revoked"] as? Bool ?? false }
  public struct SignInResult: Sendable { public var account: ChatGPTAccountInfo; public var welcome: Bool }
  public func signIn(clientId: String? = nil, consent: Bool = false, open: @Sendable (URL) async throws -> Void) async throws -> SignInResult {
    var fields: [String: Any] = ["profile": profile, "issuer": endpoints.issuer, "resource": endpoints.resource, "consent": consent]
    if let clientId { fields["client_id"] = clientId }
    let reply = try await CoreAgentClient.shared.call("SignInBeginRequest", fields)
    guard let value = reply["value"] as? [String: Any], let attempt = value["attempt_id"] as? String,
          let text = value["url"] as? String, let url = URL(string: text) else { throw ChatGPTError.failed("Invalid sign-in reply") }
    do {
      try await open(url)
      while true {
        try Task.checkCancellation()
        let status = try await request(attempt: attempt)
        switch status["state"] as? String {
        case "complete":
          guard let account = infos(status).first(where: { $0.active }) else { throw ChatGPTError.failed("No active account") }
          return SignInResult(account: account, welcome: status["welcome"] as? String == account.clientId)
        case "cancelled": throw CancellationError()
        case "failed": throw ChatGPTError.failed(status["message"] as? String ?? "Sign-in failed")
        default: try await Task.sleep(for: .milliseconds(200))
        }
      }
    } catch {
      _ = try? await request("cancel", attempt: attempt)
      throw error
    }
  }
}
