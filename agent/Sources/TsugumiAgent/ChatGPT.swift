import CryptoKit
import Foundation
import Security

// Sign in with ChatGPT for open-source tools: the Agent uses the user's
// ChatGPT plan for its Responses API requests
// (developers.openai.com/siwc/token-sharing-open-source).
//
// The app keeps its host ID and registrations in one login keychain item
// (service app.tsugumi.agent, account "ChatGPT"); the web UI's server is an
// agent host of its own (web/server/chatgpt.ts). A refresh rotates the
// refresh token, so refreshes are serialized across the app's processes with
// a lock folder, and the record is read again under the lock.

/// Where the sign-in happens (OpenAI, or the local fixture in tests).
public struct ChatGPTEndpoints: Codable, Equatable, Sendable {
  public var issuer: String
  public var authorize: URL
  public var token: URL
  public var revoke: URL
  public var jwks: URL
  /// The resource the tokens are for (the Responses API).
  public var resource: String

  public init(issuer: String, resource: String) {
    let base = URL(string: issuer)!
    self.issuer = issuer
    authorize = base.appendingPathComponent("api/accounts/authorize")
    token = base.appendingPathComponent("api/accounts/oauth/token")
    revoke = base.appendingPathComponent("api/accounts/oauth/revoke")
    jwks = base.appendingPathComponent(".well-known/jwks.json")
    self.resource = resource
  }

  public static let openAI = ChatGPTEndpoints(issuer: "https://auth.openai.com", resource: "https://api.openai.com/v1")
}

/// One registration: the client a ChatGPT account authorized for Tsugumi,
/// with its verified identity and tokens (the record of the sign-in guide).
public struct ChatGPTAccount: Codable, Equatable, Sendable {
  public var email: String?
  public var issuer: String
  public var subject: String
  public var clientId: String
  public var hostId: String
  public var idToken: String?
  public var accessToken: String?
  public var refreshToken: String?
  public var tokenType: String?
  public var expiresIn: Int?
  public var scopes: [String]
  /// When the token response was received (ISO 8601, UTC).
  public var savedAt: String?
  /// "You're using your ChatGPT plan" was shown for this registration.
  public var welcomed: Bool?

  enum CodingKeys: String, CodingKey {
    case email, issuer, subject, scopes, welcomed
    case clientId = "client_id", hostId = "ext_agent_host_id", idToken = "id_token", accessToken = "access_token"
    case refreshToken = "refresh_token", tokenType = "token_type", expiresIn = "expires_in", savedAt = "saved_at"
  }

  public var signedIn: Bool { refreshToken != nil && accessToken != nil }
  public var planEnabled: Bool { scopes.contains(ChatGPTAuth.planScope) }

  /// The access token ends within five minutes (or its expiry is unknown).
  func expiresSoon(now: Date = Date()) -> Bool {
    guard let s = savedAt, let at = ISO8601DateFormatter().date(from: s), let e = expiresIn else { return true }
    return at.addingTimeInterval(TimeInterval(e - 300)) < now
  }
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
  /// No signed-in account, or its session ended (revoked or expired): sign in again.
  case signInRequired(String)
  /// Signed in, but the use of the ChatGPT plan was not granted.
  case planNotEnabled
  case usageLimit
  case notEligible(String)
  case unsupported(String)
  /// The user declined in the browser.
  case denied
  case failed(String)

  public var description: String {
    switch self {
    case .signInRequired(let s): return "ChatGPT: sign in again in Settings > API (\(s))"
    case .planNotEnabled: return "ChatGPT: the use of your ChatGPT plan is not enabled for Tsugumi; enable it in Settings > API"
    case .usageLimit: return Self.usageLimitText
    case .notEligible(let s): return "ChatGPT: your plan cannot be used in Tsugumi (\(s))"
    case .unsupported(let s): return "ChatGPT: the request used something your plan does not support (\(s))"
    case .denied: return "ChatGPT: the sign-in was declined"
    case .failed(let s): return "ChatGPT: \(s)"
    }
  }

  /// The chat shows "Manage usage" with this error.
  public static let usageLimitText = "ChatGPT usage limit reached: review your plan or this app's limit in ChatGPT settings"
}

public actor ChatGPTAuth {
  public static let planScope = "chatgpt.tokens.use.direct"
  static let scope = "openid profile email offline_access resource.invoke \(planScope)"
  /// ChatGPT settings > Usage.
  public static let usageURL = URL(string: "https://chatgpt.com/settings/usage")!
  /// The app's name in the registration (the user can rename it in the browser).
  static let agentName = "Tsugumi"

  public let endpoints: ChatGPTEndpoints
  /// The keychain account of the record.
  public let storeAccount: String
  let lock: URL
  let session: URLSession

  private static let registry = NSLock()
  nonisolated(unsafe) private static var instances: [String: ChatGPTAuth] = [:]

  /// One instance per record, so refreshes in this process are serialized.
  public static func shared(endpoints: ChatGPTEndpoints = .openAI, store: String = "ChatGPT") -> ChatGPTAuth {
    registry.lock()
    defer { registry.unlock() }
    let key = endpoints.issuer + "\n" + store
    if let a = instances[key] { return a }
    let a = ChatGPTAuth(endpoints: endpoints, store: store)
    instances[key] = a
    return a
  }

  init(endpoints: ChatGPTEndpoints, store: String) {
    self.endpoints = endpoints
    self.storeAccount = store
    let caches = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Caches/app.tsugumi.agent")
    lock = caches.appendingPathComponent("\(store).lock")
    let c = URLSessionConfiguration.ephemeral
    // Every request ends well before a lock counts as stale (60 s).
    c.timeoutIntervalForRequest = 15
    c.timeoutIntervalForResource = 15
    session = URLSession(configuration: c)
  }

  // MARK: Record

  struct Store: Codable {
    var hostId: String
    var active: String?
    var accounts: [ChatGPTAccount] = []
    enum CodingKeys: String, CodingKey { case hostId = "ext_agent_host_id", active = "active_client_id", accounts }
  }

  private func load(asking: Bool = true) -> Store? {
    guard let s = asking ? Keychain.load(account: storeAccount) : Keychain.loadWithoutAsking(account: storeAccount) else { return nil }
    return try? JSONDecoder().decode(Store.self, from: Data(s.utf8))
  }

  private func save(_ s: Store) throws {
    let data = try JSONEncoder().encode(s)
    try Keychain.save(String(decoding: data, as: UTF8.self), account: storeAccount)
  }

  /// The record, with this host's ID made and kept before its first sign-in
  /// (an opaque urn:uuid, never derived from the user).
  private func loadOrCreate() async throws -> Store {
    if let s = load() { return s }
    return try await locked {
      if let s = load() { return s }
      let s = Store(hostId: "urn:uuid:" + UUID().uuidString.lowercased())
      try save(s)
      return s
    }
  }

  /// Serializes changes of the record with the other process (mkdir is atomic;
  /// a lock older than a minute was left by a process that ended).
  private func locked<T>(_ body: () async throws -> T) async throws -> T {
    try FileManager.default.createDirectory(at: lock.deletingLastPathComponent(), withIntermediateDirectories: true)
    let deadline = Date().addingTimeInterval(30)
    while mkdir(lock.path, 0o700) != 0 {
      guard errno == EEXIST else { throw ChatGPTError.failed("cannot lock \(lock.path) (errno \(errno))") }
      if let at = (try? FileManager.default.attributesOfItem(atPath: lock.path))?[.modificationDate] as? Date,
         at < Date().addingTimeInterval(-60) {
        rmdir(lock.path)
        continue
      }
      guard Date() < deadline else { throw ChatGPTError.failed("another Tsugumi process holds \(lock.path)") }
      try await Task.sleep(for: .milliseconds(100))
    }
    defer { rmdir(lock.path) }
    return try await body()
  }

  private func update(_ change: (inout Store) throws -> Void) async throws {
    try await locked {
      guard var s = load() else { throw ChatGPTError.signInRequired("no ChatGPT account") }
      try change(&s)
      try save(s)
    }
  }

  // MARK: Accounts

  public func accounts() -> [ChatGPTAccountInfo] {
    guard let s = load() else { return [] }
    return s.accounts.map {
      ChatGPTAccountInfo(clientId: $0.clientId, email: $0.email, signedIn: $0.signedIn, planEnabled: $0.planEnabled,
                         active: $0.clientId == s.active)
    }
  }

  /// The accounts, read without the keychain's access dialog; nil when the
  /// record is there but cannot be read without it.
  public func accountsWithoutAsking() -> [ChatGPTAccountInfo]? {
    guard Keychain.exists(account: storeAccount) else { return [] }
    guard let s = load(asking: false) else { return nil }
    return s.accounts.map {
      ChatGPTAccountInfo(clientId: $0.clientId, email: $0.email, signedIn: $0.signedIn, planEnabled: $0.planEnabled,
                         active: $0.clientId == s.active)
    }
  }

  public func select(clientId: String) async throws {
    try await update { s in
      guard s.accounts.contains(where: { $0.clientId == clientId }) else { throw ChatGPTError.failed("unknown account") }
      s.active = clientId
    }
  }

  public func markWelcomed(clientId: String) async throws {
    try await update { s in
      if let i = s.accounts.firstIndex(where: { $0.clientId == clientId }) { s.accounts[i].welcomed = true }
    }
  }

  // MARK: Sign-in

  public struct SignInResult: Sendable {
    public var account: ChatGPTAccountInfo
    /// The first sign-in of this registration with the plan enabled: show the welcome.
    public var welcome: Bool
  }

  /// Signs in with the system browser and a loopback callback. `clientId`
  /// signs in again to a saved registration; nil registers a new one.
  /// `consent` asks again for the permissions (to enable the plan later).
  public func signIn(clientId: String? = nil, consent: Bool = false,
                     open: @Sendable (URL) async throws -> Void) async throws -> SignInResult {
    let store = try await loadOrCreate()
    let existing = clientId.flatMap { id in store.accounts.first { $0.clientId == id } }
    if clientId != nil && existing == nil { throw ChatGPTError.failed("unknown account") }
    let listener = try LoopbackListener()
    let redirect = "http://127.0.0.1:\(listener.port)/auth/callback"
    let state = Self.random(), nonce = Self.random(), verifier = Self.random(48)
    let challenge = Self.base64url(Data(SHA256.hash(data: Data(verifier.utf8))))
    var q: [(String, String)] = [
      ("client_id", existing?.clientId ?? "dynamic_agent_client"),
      ("ext_agent_host_id", existing?.hostId ?? store.hostId),
      ("response_type", "code"), ("redirect_uri", redirect), ("scope", Self.scope), ("resource", endpoints.resource),
      ("state", state), ("nonce", nonce), ("code_challenge_method", "S256"), ("code_challenge", challenge),
    ]
    if let e = existing {
      if let t = e.idToken { q.append(("id_token_hint", t)) }
      if let m = e.email { q.append(("login_hint", m)) }
    } else {
      q.append(("agent_name_hint", Self.agentName))
    }
    if consent { q.append(("prompt", "consent")) }
    // The listener answers before the browser opens (a browser may wait for the callback's page).
    let waiting = Task { try await listener.callback(state: state, timeout: 600) }
    let cb: [String: String]
    do {
      try await open(Self.url(endpoints.authorize, q))
      cb = try await withTaskCancellationHandler { try await waiting.value } onCancel: { waiting.cancel() }
    } catch {
      waiting.cancel()
      throw error
    }

    guard cb["state"] == state else { throw ChatGPTError.failed("the sign-in answer did not match its request (state)") }
    if let e = cb["error"] {
      if e == "access_denied" { throw ChatGPTError.denied }
      throw ChatGPTError.failed("\(e): \(cb["error_description"] ?? "")")
    }
    let issued: String
    if let e = existing {
      if let c = cb["client_id"], c != e.clientId { throw ChatGPTError.failed("the sign-in returned another registration") }
      issued = e.clientId
    } else {
      guard let c = cb["client_id"], !c.isEmpty, c != "dynamic_agent_client" else {
        throw ChatGPTError.failed("the registration did not finish (no client ID)")
      }
      issued = c
    }
    guard let code = cb["code"], !code.isEmpty else { throw ChatGPTError.failed("the sign-in returned no code") }

    let token = try await tokenRequest([
      ("grant_type", "authorization_code"), ("client_id", issued), ("code", code), ("code_verifier", verifier),
      ("redirect_uri", redirect), ("resource", endpoints.resource),
    ])
    guard let idToken = token["id_token"] as? String, let access = token["access_token"] as? String,
          let refresh = token["refresh_token"] as? String else {
      throw ChatGPTError.failed("the token response lacks its tokens")
    }
    let claims = try await verify(idToken: idToken, audience: issued, nonce: nonce)
    guard let sub = claims["sub"] as? String, !sub.isEmpty else { throw ChatGPTError.failed("the ID token has no subject") }
    if let e = existing, e.subject != sub {
      throw ChatGPTError.failed("you signed in to another ChatGPT account than \(e.email ?? "the selected one")")
    }
    let scopes = ((token["scope"] as? String) ?? cb["scope"] ?? "").split(separator: " ").map(String.init).sorted()
    var account = ChatGPTAccount(
      email: claims["email"] as? String ?? existing?.email, issuer: endpoints.issuer, subject: sub, clientId: issued,
      hostId: existing?.hostId ?? store.hostId, idToken: idToken, accessToken: access, refreshToken: refresh,
      tokenType: token["token_type"] as? String ?? "Bearer", expiresIn: token["expires_in"] as? Int ?? 3600,
      scopes: scopes, savedAt: ISO8601DateFormatter().string(from: Date()), welcomed: existing?.welcomed)
    let welcome = account.planEnabled && account.welcomed != true
    // A new sign-in becomes the active account only now that it is validated.
    try await update { s in
      if let i = s.accounts.firstIndex(where: { $0.clientId == issued }) {
        account.welcomed = s.accounts[i].welcomed
        s.accounts[i] = account
      } else {
        s.accounts.append(account)
      }
      s.active = issued
    }
    return SignInResult(account: ChatGPTAccountInfo(clientId: issued, email: account.email, signedIn: true,
                                                    planEnabled: account.planEnabled, active: true),
                        welcome: welcome)
  }

  /// Signs out: revokes the refresh token, then forgets the tokens (the
  /// registration and the host ID stay for a later sign-in). False when the
  /// revocation was not confirmed (the user can disconnect Tsugumi in ChatGPT
  /// settings).
  public func signOut(clientId: String) async throws -> Bool {
    // Under the lock, so a refresh cannot rotate the token between its
    // revocation and the forgetting of the tokens.
    try await locked {
      guard var s = load(), let i = s.accounts.firstIndex(where: { $0.clientId == clientId }) else { return true }
      var revoked = s.accounts[i].refreshToken == nil
      if let t = s.accounts[i].refreshToken {
        for attempt in 0..<3 where !revoked {
          if attempt > 0 { try? await Task.sleep(for: .seconds(Double(attempt))) }
          var r = URLRequest(url: endpoints.revoke)
          r.httpMethod = "POST"
          r.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "content-type")
          r.httpBody = Data(Self.form([("token", t), ("token_type_hint", "refresh_token"), ("client_id", clientId)]).utf8)
          guard let (_, resp) = try? await session.data(for: r), let status = (resp as? HTTPURLResponse)?.statusCode else { continue }
          if status == 200 { revoked = true } else if status < 500 { break }
        }
      }
      s.accounts[i].accessToken = nil
      s.accounts[i].refreshToken = nil
      s.accounts[i].idToken = nil
      s.accounts[i].expiresIn = nil
      s.accounts[i].savedAt = nil
      try save(s)
      return revoked
    }
  }

  // MARK: Tokens

  public struct AccessToken: Sendable {
    public let clientId: String
    public let value: String
  }

  /// The active account's access token, refreshed near its expiry. `stale`
  /// is a token the API refused: it is refreshed unless another refresh
  /// already replaced it. Without `asking` the record is read without the
  /// keychain's access dialog (it fails instead, and nothing changes).
  public func accessToken(replacing stale: AccessToken? = nil, asking: Bool = true) async throws -> AccessToken {
    guard let s = load(asking: asking), let id = s.active, let a = s.accounts.first(where: { $0.clientId == id }) else {
      throw ChatGPTError.signInRequired("no ChatGPT account is signed in")
    }
    if let stale, stale.clientId != id { throw ChatGPTError.failed("the active account changed; try again") }
    guard a.signedIn, let current = a.accessToken else { throw ChatGPTError.signInRequired("signed out") }
    guard a.planEnabled else { throw ChatGPTError.planNotEnabled }
    let needsRefresh = stale.map { $0.value == current } ?? a.expiresSoon()
    if !needsRefresh { return AccessToken(clientId: id, value: current) }
    // Not cancelled with the turn: once OpenAI rotated the refresh token, the
    // answer must be saved, or the next refresh reuses the old token and the
    // account is signed out.
    let value = try await Task { try await self.refreshLocked(id: id, stale: stale?.value, asking: asking) }.value
    return AccessToken(clientId: id, value: value)
  }

  private func refreshLocked(id: String, stale: String?, asking: Bool) async throws -> String {
    try await locked {
      // Read again: the other process may have refreshed meanwhile.
      guard let s = load(asking: asking), let a = s.accounts.first(where: { $0.clientId == id }), let current = a.accessToken,
            let refresh = a.refreshToken else {
        throw ChatGPTError.signInRequired("signed out")
      }
      guard s.active == id else { throw ChatGPTError.failed("the active account changed; try again") }
      guard a.planEnabled else { throw ChatGPTError.planNotEnabled }
      if stale == nil ? !a.expiresSoon() : current != stale { return current }
      let r = try await refreshRequest(clientId: id, refreshToken: refresh)
      // May ask whatever the caller said: the rotated refresh token must be saved.
      guard var s2 = load(), let i = s2.accounts.firstIndex(where: { $0.clientId == id }) else {
        throw ChatGPTError.signInRequired("signed out")
      }
      switch r {
      case .tokens(let t):
        guard let access = t["access_token"] as? String else { throw ChatGPTError.failed("the refresh returned no access token") }
        // The access token, its expiry, the scopes and the rotating refresh token change together.
        s2.accounts[i].accessToken = access
        s2.accounts[i].refreshToken = t["refresh_token"] as? String ?? refresh
        s2.accounts[i].expiresIn = t["expires_in"] as? Int ?? 3600
        if let sc = t["scope"] as? String { s2.accounts[i].scopes = sc.split(separator: " ").map(String.init).sorted() }
        if let idt = t["id_token"] as? String { s2.accounts[i].idToken = idt }
        s2.accounts[i].savedAt = ISO8601DateFormatter().string(from: Date())
        try save(s2)
        guard s2.accounts[i].planEnabled else { throw ChatGPTError.planNotEnabled }
        return access
      case .ended(let code):
        // The refresh token can no longer be used: sign in again with the same registration.
        s2.accounts[i].accessToken = nil
        s2.accounts[i].refreshToken = nil
        try save(s2)
        throw ChatGPTError.signInRequired(code)
      }
    }
  }

  enum Refresh { case tokens([String: Any]), ended(String) }
  static let endedCodes: Set<String> = ["invalid_grant", "invalid_refresh_token", "token_expired", "refresh_token_expired",
                                        "refresh_token_invalidated", "refresh_token_reused"]

  private func refreshRequest(clientId: String, refreshToken: String) async throws -> Refresh {
    var r = URLRequest(url: endpoints.token)
    r.httpMethod = "POST"
    r.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "content-type")
    r.httpBody = Data(Self.form([("grant_type", "refresh_token"), ("client_id", clientId), ("refresh_token", refreshToken),
                                 ("resource", endpoints.resource)]).utf8)
    let (data, resp) = try await session.data(for: r)
    let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
    let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
    if status == 200 { return .tokens(obj) }
    // A failure of the network or of the server keeps the tokens (retried later).
    if status >= 500 || status == 429 {
      throw MessagesProvider.HTTPStatusError(status: status, body: String(decoding: data, as: UTF8.self))
    }
    let code = Self.errorCode(obj) ?? "HTTP \(status)"
    if Self.endedCodes.contains(code) { return .ended(code) }
    throw ChatGPTError.failed("token refresh failed: \(code)")
  }

  private func tokenRequest(_ form: [(String, String)]) async throws -> [String: Any] {
    var r = URLRequest(url: endpoints.token)
    r.httpMethod = "POST"
    r.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "content-type")
    r.httpBody = Data(Self.form(form).utf8)
    let (data, resp) = try await session.data(for: r)
    let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
    guard (resp as? HTTPURLResponse)?.statusCode == 200 else {
      throw ChatGPTError.failed("the code exchange failed: \(Self.errorCode(obj) ?? "HTTP \((resp as? HTTPURLResponse)?.statusCode ?? 0)"); sign in again")
    }
    return obj
  }

  static func errorCode(_ obj: [String: Any]) -> String? {
    if let e = obj["error"] as? String { return e }
    if let e = obj["error"] as? [String: Any] { return e["code"] as? String ?? e["type"] as? String }
    return obj["code"] as? String
  }

  // MARK: ID token

  /// Verifies the ID token's RS256 signature with the issuer's JWKS, then its
  /// issuer, audience (the issued client ID), expiry and nonce.
  func verify(idToken: String, audience: String, nonce: String) async throws -> [String: Any] {
    let parts = idToken.split(separator: ".", omittingEmptySubsequences: false).map(String.init)
    guard parts.count == 3, let h = Self.unbase64url(parts[0]), let p = Self.unbase64url(parts[1]),
          let sig = Self.unbase64url(parts[2]),
          let header = (try? JSONSerialization.jsonObject(with: h)) as? [String: Any],
          let claims = (try? JSONSerialization.jsonObject(with: p)) as? [String: Any] else {
      throw ChatGPTError.failed("the ID token is malformed")
    }
    guard header["alg"] as? String == "RS256" else { throw ChatGPTError.failed("the ID token is not signed with RS256") }
    let (data, _) = try await session.data(from: endpoints.jwks)
    let keys = ((try? JSONSerialization.jsonObject(with: data)) as? [String: Any])?["keys"] as? [[String: Any]] ?? []
    let kid = header["kid"] as? String
    guard let jwk = keys.first(where: { $0["kty"] as? String == "RSA" && (kid == nil || $0["kid"] as? String == kid) }),
          let n = (jwk["n"] as? String).flatMap(Self.unbase64url), let e = (jwk["e"] as? String).flatMap(Self.unbase64url),
          let key = SecKeyCreateWithData(Self.rsaPublicKeyDER(n: n, e: e) as CFData,
                                         [kSecAttrKeyType: kSecAttrKeyTypeRSA, kSecAttrKeyClass: kSecAttrKeyClassPublic] as CFDictionary, nil)
    else { throw ChatGPTError.failed("no key of the issuer verifies the ID token") }
    guard SecKeyVerifySignature(key, .rsaSignatureMessagePKCS1v15SHA256, Data((parts[0] + "." + parts[1]).utf8) as CFData,
                                sig as CFData, nil) else {
      throw ChatGPTError.failed("the ID token's signature is not valid")
    }
    guard claims["iss"] as? String == endpoints.issuer else { throw ChatGPTError.failed("the ID token has another issuer") }
    let aud = (claims["aud"] as? [String]) ?? [(claims["aud"] as? String) ?? ""]
    guard aud.contains(audience) else { throw ChatGPTError.failed("the ID token is for another client") }
    guard let exp = (claims["exp"] as? NSNumber)?.doubleValue, Date(timeIntervalSince1970: exp + 120) > Date() else {
      throw ChatGPTError.failed("the ID token has expired")
    }
    guard claims["nonce"] as? String == nonce else { throw ChatGPTError.failed("the ID token's nonce does not match") }
    return claims
  }

  /// PKCS#1 RSAPublicKey: SEQUENCE { INTEGER n, INTEGER e }.
  static func rsaPublicKeyDER(n: Data, e: Data) -> Data {
    func length(_ l: Int) -> Data {
      if l < 0x80 { return Data([UInt8(l)]) }
      var bytes: [UInt8] = []
      var v = l
      while v > 0 { bytes.insert(UInt8(v & 0xff), at: 0); v >>= 8 }
      return Data([0x80 | UInt8(bytes.count)] + bytes)
    }
    func integer(_ d: Data) -> Data {
      var b = Data(d.drop(while: { $0 == 0 }))
      if b.isEmpty || b.first! & 0x80 != 0 { b.insert(0, at: 0) }
      return Data([0x02]) + length(b.count) + b
    }
    let body = integer(n) + integer(e)
    return Data([0x30]) + length(body.count) + body
  }

  // MARK: Encoding

  static func random(_ bytes: Int = 32) -> String {
    var b = [UInt8](repeating: 0, count: bytes)
    _ = SecRandomCopyBytes(kSecRandomDefault, bytes, &b)
    return base64url(Data(b))
  }

  static func base64url(_ d: Data) -> String {
    d.base64EncodedString().replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_")
      .replacingOccurrences(of: "=", with: "")
  }

  static func unbase64url(_ s: String) -> Data? {
    var t = s.replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
    while t.count % 4 != 0 { t += "=" }
    return Data(base64Encoded: t)
  }

  /// Form and query encoding: every value percent-encoded (RFC 3986 unreserved kept).
  static func form(_ items: [(String, String)]) -> String {
    let unreserved = CharacterSet(charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")
    return items.map { "\($0.0)=\($0.1.addingPercentEncoding(withAllowedCharacters: unreserved) ?? "")" }.joined(separator: "&")
  }

  static func url(_ base: URL, _ items: [(String, String)]) -> URL {
    URL(string: base.absoluteString + "?" + form(items))!
  }
}

/// A one-shot HTTP listener on 127.0.0.1 for the OAuth callback
/// (/auth/callback with the sign-in's state); anything else gets 404 and it
/// keeps waiting.
final class LoopbackListener: @unchecked Sendable {
  let fd: Int32
  let port: Int
  private let stop = NSLock()
  private var stopped = false

  init() throws {
    let s = socket(AF_INET, SOCK_STREAM, 0)
    guard s >= 0 else { throw ChatGPTError.failed("cannot open the sign-in listener") }
    var a = sockaddr_in()
    a.sin_family = sa_family_t(AF_INET)
    a.sin_addr.s_addr = inet_addr("127.0.0.1")
    a.sin_port = 0
    var len = socklen_t(MemoryLayout<sockaddr_in>.size)
    let bound = withUnsafeMutablePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(s, $0, len) } }
    guard bound == 0, listen(s, 8) == 0 else {
      close(s)
      throw ChatGPTError.failed("cannot listen on 127.0.0.1 for the sign-in")
    }
    _ = withUnsafeMutablePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(s, $0, &len) } }
    fd = s
    port = Int(UInt16(bigEndian: a.sin_port))
  }

  deinit { close(fd) }

  private var isStopped: Bool {
    stop.lock()
    defer { stop.unlock() }
    return stopped
  }

  /// The callback's query parameters. A callback with another state (a page
  /// or process that found the port) gets 404, and the wait goes on.
  func callback(state: String, timeout: TimeInterval) async throws -> [String: String] {
    try await withTaskCancellationHandler {
      try await Task.detached { [self] in try wait(state: state, until: Date().addingTimeInterval(timeout)) }.value
    } onCancel: {
      stop.lock()
      stopped = true
      stop.unlock()
    }
  }

  private func wait(state: String, until deadline: Date) throws -> [String: String] {
    while true {
      if isStopped { throw CancellationError() }
      guard Date() < deadline else { throw ChatGPTError.failed("the sign-in was not finished in the browser in time") }
      var p = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
      guard poll(&p, 1, 300) > 0 else { continue }
      let c = accept(fd, nil, nil)
      guard c >= 0 else { continue }
      defer { close(c) }
      var tv = timeval(tv_sec: 5, tv_usec: 0)
      setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
      var nosig: Int32 = 1
      setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &nosig, socklen_t(MemoryLayout<Int32>.size))
      var req = Data()
      var buf = [UInt8](repeating: 0, count: 4096)
      while req.count < 32768, !req.contains(Data("\r\n\r\n".utf8)) {
        let n = recv(c, &buf, buf.count, 0)
        if n <= 0 { break }
        req.append(buf, count: n)
      }
      let line = String(decoding: req.prefix { $0 != 0x0d && $0 != 0x0a }, as: UTF8.self).split(separator: " ")
      guard line.count >= 2, line[0] == "GET", let comps = URLComponents(string: "http://127.0.0.1" + line[1]),
            comps.path == "/auth/callback" else {
        respond(c, status: "404 Not Found", body: "Not found")
        continue
      }
      var q: [String: String] = [:]
      for item in comps.queryItems ?? [] { q[item.name] = item.value ?? "" }
      guard q["state"] == state else {
        respond(c, status: "404 Not Found", body: "Not found")
        continue
      }
      let ok = q["error"] == nil
      respond(c, status: "200 OK", body: ok
        ? "サインインしました。このタブを閉じて Tsugumi に戻ってください。 / Signed in. Close this tab and return to Tsugumi."
        : "サインインは完了しませんでした。Tsugumi に戻ってください。 / The sign-in did not finish. Return to Tsugumi.")
      return q
    }
  }

  private func respond(_ c: Int32, status: String, body: String) {
    let html = "<!doctype html><meta charset=utf-8><title>Tsugumi</title><body style=\"font: 15px -apple-system, sans-serif; margin: 3em\"><p>\(body)</p>"
    let head = "HTTP/1.1 \(status)\r\ncontent-type: text/html; charset=utf-8\r\ncontent-length: \(html.utf8.count)\r\n"
      + "cache-control: no-store\r\nreferrer-policy: no-referrer\r\nconnection: close\r\n\r\n"
    let d = Array((head + html).utf8)
    _ = d.withUnsafeBytes { send(c, $0.baseAddress, $0.count, 0) }
  }
}
