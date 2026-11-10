import Foundation
import Security

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

  /// The search in words, for a request that cannot carry the provider's
  /// blocks (the tool off, another provider). It never names the tool.
  var note: String {
    let results = sources.isEmpty ? "" : "; results: " + sources.map(\.url).joined(separator: ", ")
    return "[web search: \(query)\(error.map { "; failed: \($0)" } ?? "")\(results)]"
  }

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

/// One content block of a chat message (Anthropic Messages wire shape).
public enum ContentBlock: Codable, Equatable {
  case text(String)
  case toolUse(id: String, name: String, input: String)  // input: JSON text
  case toolResult(toolUseId: String, content: String, isError: Bool, images: [ToolImage] = [])
  case webSearch(WebSearch)

  enum CodingKeys: String, CodingKey { case type, text, id, name, input, tool_use_id, content, is_error, images, search }

  public init(from d: Decoder) throws {
    let c = try d.container(keyedBy: CodingKeys.self)
    switch try c.decode(String.self, forKey: .type) {
    case "text": self = .text(try c.decode(String.self, forKey: .text))
    case "tool_use": self = .toolUse(id: try c.decode(String.self, forKey: .id), name: try c.decode(String.self, forKey: .name),
                                     input: try c.decode(String.self, forKey: .input))
    case "web_search": self = .webSearch(try c.decode(WebSearch.self, forKey: .search))
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
      try c.encode(s, forKey: .search)
    }
  }

  /// The arguments of a tool call as the object they must be; nil: broken JSON
  /// or something else than an object (the API does not check them as they
  /// stream). A broken call goes back as an empty one: the APIs want an object
  /// there, and its text is in its result.
  public static func arguments(_ input: String) -> [String: Any]? {
    (try? JSONSerialization.jsonObject(with: Data(input.utf8))) as? [String: Any]
  }

  /// Wire JSON for the Messages API.
  var wire: [String: Any] {
    switch self {
    case .text(let t): return ["type": "text", "text": t]
    case .toolUse(let id, let name, let input):
      let obj = Self.arguments(input) ?? [:]
      return ["type": "tool_use", "id": id, "name": name, "input": obj]
    case .toolResult(let id, let content, let isError, let images):
      let shown: [[String: Any]] = images.map {
        ["type": "image", "source": ["type": "base64", "media_type": $0.mediaType, "data": $0.data]]
      }
      let sent: Any = images.isEmpty ? content : [["type": "text", "text": content]] + shown
      return ["type": "tool_result", "tool_use_id": id, "content": sent, "is_error": isError]
    case .webSearch(let s): return ["type": "text", "text": s.note]
    }
  }
}

public struct ChatMessage: Codable, Equatable {
  public var role: String  // "user" | "assistant"
  public var content: [ContentBlock]
  /// What the chat shows for a request the app wrote for the model (the
  /// model gets `content`; this is never sent).
  public var display: String?
  /// A reply with search blocks as the Messages API sent them (JSON text of
  /// the content list): it wants them back unchanged (encrypted results,
  /// citations) while the search tool is declared. `content` says the same in
  /// the app's own terms.
  public var providerContent: String?
  public init(role: String, content: [ContentBlock], display: String? = nil, providerContent: String? = nil) {
    self.role = role
    self.content = content
    self.display = display
    self.providerContent = providerContent
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

public enum StreamEvent {
  case text(String)
  case toolStart(index: Int, id: String, name: String)
  case toolInput(index: Int, partialJSON: String)
  case blockStop(index: Int)
  /// A search the provider runs, and its result (at once, or in a later reply).
  case search(id: String, query: String)
  case searchResult(id: String, sources: [WebSource], error: String?)
  /// A source the reply cites.
  case citation(WebSource)
  /// The reply's blocks as JSON text, when only they carry all of it.
  case providerContent(String)
  case stop(reason: String, usage: Usage)
}

public struct ProviderConfig: Codable, Equatable, Sendable {
  /// Wire protocol of the endpoint.
  public enum API: String, Codable, Sendable { case anthropicMessages, openAIResponses }
  /// How requests are authorized: none (the local fixture), an API key in
  /// the Keychain, or the user's ChatGPT plan (Sign in with ChatGPT).
  public enum Auth: String, Codable, Sendable { case none, apiKey, chatGPT }

  public var name: String
  public var baseURL: URL
  public var model: String
  public var auth: Auth
  public var api: API = .anthropicMessages
  /// Output limit of one model reply: a Card's sources fit in one save
  /// (not sent with the ChatGPT plan, which does not accept it).
  public var maxTokens = 32768
  public var contextLimit = 180_000
  /// ChatGPT sign-in: where it signs in and the keychain item of its accounts.
  public var chatGPTEndpoints = ChatGPTEndpoints.openAI
  public var chatGPTStore = "ChatGPT"
  /// The user's setting (Settings > API): the model may search the web.
  public var webSearch = false
  /// The chat's choices in the model menu, already checked against the model
  /// (`choosing`). None for the requests that are not the Agent's work.
  public var effort: String?
  public var fast = false

  public init(name: String, baseURL: URL, model: String, auth: Auth, api: API = .anthropicMessages) {
    self.name = name
    self.baseURL = baseURL
    self.model = model
    self.auth = auth
    self.api = api
  }

  /// A real provider (not the local fixture).
  public var isRemote: Bool { auth != .none }
  /// The provider runs the searches itself: the local fixture has none.
  public var searchesWeb: Bool { webSearch && isRemote }

  public static let anthropic = ProviderConfig(name: "Anthropic", baseURL: URL(string: "https://api.anthropic.com")!,
                                               model: "claude-sonnet-5-5", auth: .apiKey)
  /// OpenAI with an API key (Responses API).
  public static let openAI = ProviderConfig(name: "OpenAI", baseURL: URL(string: "https://api.openai.com")!,
                                            model: "gpt-6.1-sol", auth: .apiKey, api: .openAIResponses)
  /// OpenAI with the user's ChatGPT plan (Sign in with ChatGPT, open-source flow).
  public static let chatGPT = ProviderConfig(name: "ChatGPT", baseURL: URL(string: "https://api.openai.com")!,
                                             model: "gpt-6.1-sol", auth: .chatGPT, api: .openAIResponses)
  public static func localFixture(port: Int) -> ProviderConfig {
    var c = ProviderConfig(name: "Local fixture", baseURL: URL(string: "http://127.0.0.1:\(port)")!, model: "fixture", auth: .none)
    c.contextLimit = 6000
    return c
  }

  var chatGPTAuth: ChatGPTAuth { ChatGPTAuth.shared(endpoints: chatGPTEndpoints, store: chatGPTStore) }
}

/// Model credentials stay in the user's login keychain on the host (one item
/// per provider, service app.tsugumi.agent and the provider's name as the
/// account, the same item the web UI's server reads with `security`). The
/// item's access list trusts the program that saved it; others are asked
/// about first. They are never written to project files, settings or logs,
/// and never sent to tsugumid or to Card workers.
///
/// Not the data protection keychain (kSecUseDataProtectionKeychain): it needs
/// a keychain-access-groups entitlement, which only a provisioning profile
/// grants, and an ad-hoc signed build from source has none (errSecMissingEntitlement).
/// Login keychain items are never synced to iCloud Keychain.
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

/// Messages API client with SSE streaming. The same code talks to the real
/// provider and to the local fixture; only the endpoint and key differ.
public struct MessagesProvider: Sendable {
  public let config: ProviderConfig
  let session: URLSession

  /// One session for every provider: nothing of it is kept on disk, and a
  /// provider is made for each listing of models (a session is never freed).
  private static let shared: URLSession = {
    let c = URLSessionConfiguration.ephemeral
    c.timeoutIntervalForRequest = 120
    return URLSession(configuration: c)
  }()

  public init(config: ProviderConfig) {
    self.config = config
    session = Self.shared
  }

  /// A request with its credentials: the API key, or the ChatGPT access
  /// token (`stale`: one the API refused, to be refreshed). Without `asking`
  /// it fails rather than show the keychain's access dialog.
  private func request(path: String, stale: String? = nil, asking: Bool = true) async throws -> URLRequest {
    var r = URLRequest(url: config.baseURL.appendingPathComponent(path))
    r.setValue("application/json", forHTTPHeaderField: "content-type")
    if config.api == .anthropicMessages { r.setValue("2023-06-01", forHTTPHeaderField: "anthropic-version") }
    switch config.auth {
    case .none: break
    case .apiKey:
      guard let key = asking ? Keychain.load(account: config.name) : Keychain.loadWithoutAsking(account: config.name) else {
        throw AgentError.provider("no API key for \(config.name); add it in Settings")
      }
      switch config.api {
      case .anthropicMessages: r.setValue(key, forHTTPHeaderField: "x-api-key")
      case .openAIResponses: r.setValue("Bearer " + key, forHTTPHeaderField: "authorization")
      }
    case .chatGPT:
      r.setValue("Bearer " + (try await config.chatGPTAuth.accessToken(replacing: stale, asking: asking)), forHTTPHeaderField: "authorization")
    }
    return r
  }

  /// Model capability discovery (available models).
  public func models() async throws -> [String] { try await modelChoices().map(\.id) }

  /// Models with the names to show (the ChatGPT account's catalog lists
  /// display names, in the server's order). Always a new answer of the
  /// provider, within 15 s. `asking: false` is for lists nobody asked for (the
  /// model menu opened): it never shows the keychain's access dialog, and
  /// throws when the secret cannot be read without it.
  public func modelChoices(asking: Bool = true) async throws -> [(id: String, label: String)] {
    var r = try await request(path: "v1/models", asking: asking)
    r.timeoutInterval = 15
    r.cachePolicy = .reloadIgnoringLocalCacheData
    let (data, resp) = try await session.data(for: r)
    guard (resp as? HTTPURLResponse)?.statusCode == 200,
          let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
      throw AgentError.provider("cannot list models")
    }
    if config.auth == .chatGPT, let list = obj["models"] as? [[String: Any]] {
      return list.filter { $0["visibility"] as? String == "list" }.compactMap { m in
        (m["slug"] as? String).map { ($0, m["display_name"] as? String ?? $0) }
      }
    }
    guard let list = obj["data"] as? [[String: Any]] else { throw AgentError.provider("cannot list models") }
    return list.compactMap { ($0["id"] as? String).map { ($0, $0) } }
  }

  public struct HTTPStatusError: Error {
    public let status: Int
    public let body: String
    /// The provider's retry-after (seconds), when it sent one.
    public var retryAfter: TimeInterval? = nil
    public var retryable: Bool { status == 429 || status >= 500 }
  }

  static func retryAfter(_ resp: URLResponse) -> TimeInterval? {
    guard let h = resp as? HTTPURLResponse else { return nil }
    if let ms = h.value(forHTTPHeaderField: "retry-after-ms").flatMap(Double.init) { return ms / 1000 }
    return h.value(forHTTPHeaderField: "retry-after").flatMap(Double.init)
  }

  /// Prompt caching: the system prompt and the tools are the same in every
  /// step, and each step repeats the conversation before it, so all three end
  /// with a cache breakpoint (at most 4 are allowed). The rules every Agent has
  /// end with theirs, before what is an Agent's own: helpers read them from one
  /// another, which the end of the whole system prompt would not let them.
  static func cachedBody(system: String, messages: [ChatMessage], tools: [ToolSpec]) -> (system: Any, messages: Any, tools: Any) {
    let cache: [String: Any] = ["type": "ephemeral"]
    var wireTools: [[String: Any]] = tools.map {
      if $0.server { return Self.webSearchWire }
      // Eager: the arguments come as the model writes them, not held back until
      // each is whole, so the API no longer checks them (Agent.runTool does).
      return ["name": $0.name, "description": $0.description, "input_schema": $0.inputSchema, "eager_input_streaming": true]
    }
    if !wireTools.isEmpty, tools.last?.server != true { wireTools[wireTools.count - 1]["cache_control"] = cache }
    // Search blocks go back only with the tool declared, and only whole.
    let replay = tools.contains { $0.server } ? Self.replayable(messages) : []
    var wire: [[String: Any]] = messages.enumerated().map { i, m in
      if replay.contains(i), let blocks = Self.providerBlocks(m) { return ["role": m.role, "content": blocks] }
      return ["role": m.role, "content": m.content.map(\.wire)]
    }
    if var last = wire.last, var content = last["content"] as? [[String: Any]],
       let at = content.lastIndex(where: Self.cacheable) {
      content[at]["cache_control"] = cache
      last["content"] = content
      wire[wire.count - 1] = last
    }
    let shared = Shared.systemPrompt
    let blocks: [[String: Any]] = system.count > shared.count && system.hasPrefix(shared)
      ? [["type": "text", "text": shared, "cache_control": cache], ["type": "text", "text": String(system.dropFirst(shared.count))]]
      : [["type": "text", "text": system, "cache_control": cache]]
    return (blocks, wire, wireTools)
  }

  /// How long a stream may stay silent at the deep levels, instead of the
  /// session's 120 s (as long as the web Agent's fetch allows).
  static let longThinkingTimeout: TimeInterval = 300

  /// The request body of the Messages API.
  func messagesBody(_ cached: (system: Any, messages: Any, tools: Any)) -> [String: Any] {
    var body: [String: Any] = [
      "model": config.model,
      "max_tokens": config.outputLimit,
      "stream": true,
      "system": cached.system,
      "messages": cached.messages,
      "tools": cached.tools,
    ]
    if let effort = config.effort { body["output_config"] = ["effort": effort] }
    if config.fast { body["speed"] = "fast" }
    return body
  }

  /// A request that writes the cache up to `system` and is answered with nothing.
  func warmBody(system: String, tools: [ToolSpec]) -> [String: Any] {
    var body = messagesBody(Self.cachedBody(system: system, messages: [], tools: tools))
    body["stream"] = nil
    body["max_tokens"] = 0
    body["messages"] = [["role": "user", "content": "warmup"]]
    return body
  }

  /// Writes what the helpers of a delegate call share, before they start:
  /// requests sent at the same time cannot read what the others are writing.
  /// Failing costs only the saving, so nothing is retried or reported.
  func warmCache(system: String, tools: [ToolSpec]) async -> Usage {
    var usage = Usage()
    guard config.api == .anthropicMessages, var r = try? await request(path: "v1/messages"),
          let payload = try? JSONSerialization.data(withJSONObject: warmBody(system: system, tools: tools)) else { return usage }
    r.httpMethod = "POST"
    r.httpBody = payload
    r.timeoutInterval = 30
    guard let (data, resp) = try? await session.data(for: r), (resp as? HTTPURLResponse)?.statusCode == 200,
          let u = ((try? JSONSerialization.jsonObject(with: data)) as? [String: Any])?["usage"] as? [String: Any] else { return usage }
    usage.cacheReadTokens = u["cache_read_input_tokens"] as? Int ?? 0
    usage.cacheWriteTokens = u["cache_creation_input_tokens"] as? Int ?? 0
    return usage
  }

  public func stream(system: String, messages: [ChatMessage], tools: [ToolSpec]) throws -> AsyncThrowingStream<StreamEvent, Error> {
    if config.api == .openAIResponses { return try responsesStream(system: system, messages: messages, tools: tools) }
    let cached = Self.cachedBody(system: system, messages: messages, tools: tools)
    let payload = try JSONSerialization.data(withJSONObject: messagesBody(cached))
    let session = self.session
    return AsyncThrowingStream { cont in
      let task = Task {
        do {
          var r = try await request(path: "v1/messages")
          r.httpMethod = "POST"
          r.httpBody = payload
          if config.thinksLong { r.timeoutInterval = Self.longThinkingTimeout }
          let (bytes, resp) = try await session.bytes(for: r)
          let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
          if status != 200 {
            var text = ""
            for try await line in bytes.lines { text += line }
            throw HTTPStatusError(status: status, body: text, retryAfter: Self.retryAfter(resp))
          }
          var usage = Usage()
          var event = ""
          var stopped = false
          // The reply's blocks as they come, to send them back.
          var reply = ReplyBlocks()
          for try await line in bytes.lines {
            try Task.checkCancellation()
            if line.hasPrefix("event:") { event = line.dropFirst(6).trimmingCharacters(in: .whitespaces); continue }
            guard line.hasPrefix("data:") else { continue }
            let payload = Data(line.dropFirst(5).utf8)
            guard let obj = try? JSONSerialization.jsonObject(with: payload) as? [String: Any] else { continue }
            let type = obj["type"] as? String ?? event
            switch type {
            case "message_start":
              if let u = (obj["message"] as? [String: Any])?["usage"] as? [String: Any] {
                usage.cacheReadTokens = u["cache_read_input_tokens"] as? Int ?? 0
                usage.cacheWriteTokens = u["cache_creation_input_tokens"] as? Int ?? 0
                usage.inputTokens = (u["input_tokens"] as? Int ?? 0) + usage.cacheReadTokens + usage.cacheWriteTokens
              }
            case "content_block_start":
              let idx = obj["index"] as? Int ?? 0
              if let b = obj["content_block"] as? [String: Any] {
                reply.start(idx, b)
                switch b["type"] as? String {
                case "tool_use":
                  cont.yield(.toolStart(index: idx, id: b["id"] as? String ?? UUID().uuidString, name: b["name"] as? String ?? ""))
                case "web_search_tool_result":
                  let (sources, error) = Self.searchResult(b["content"])
                  cont.yield(.searchResult(id: b["tool_use_id"] as? String ?? "", sources: sources, error: error))
                default:
                  break
                }
              }
            case "content_block_delta":
              let idx = obj["index"] as? Int ?? 0
              if let d = obj["delta"] as? [String: Any] {
                if let t = d["text"] as? String {
                  reply.text(idx, t)
                  cont.yield(.text(t))
                }
                if let pj = d["partial_json"] as? String {
                  reply.input(idx, pj)
                  if !reply.isSearch(idx) { cont.yield(.toolInput(index: idx, partialJSON: pj)) }
                }
                if let c = d["citation"] as? [String: Any] {
                  reply.citation(idx, c)
                  if let url = c["url"] as? String { cont.yield(.citation(WebSource(url: url, title: c["title"] as? String))) }
                }
              }
            case "content_block_stop":
              let idx = obj["index"] as? Int ?? 0
              if let (id, query) = reply.search(idx) { cont.yield(.search(id: id, query: query)) }
              cont.yield(.blockStop(index: idx))
            case "message_stop":
              stopped = true
            case "message_delta":
              if let u = obj["usage"] as? [String: Any] { usage.outputTokens = u["output_tokens"] as? Int ?? usage.outputTokens }
              if let d = obj["delta"] as? [String: Any], let reason = d["stop_reason"] as? String {
                if let blocks = reply.toSendBack { cont.yield(.providerContent(blocks)) }
                cont.yield(.stop(reason: reason, usage: usage))
              }
            case "error":
              throw AgentError.provider((obj["error"] as? [String: Any])?["message"] as? String ?? "stream error")
            default:
              break
            }
          }
          // A stream cut before message_stop is a lost connection, not a short answer.
          if !stopped { throw URLError(.networkConnectionLost) }
          cont.finish()
        } catch {
          cont.finish(throwing: error)
        }
      }
      cont.onTermination = { _ in task.cancel() }
    }
  }
}

// MARK: - OpenAI Responses API

extension MessagesProvider {
  /// The ChatGPT plan takes function tools only grouped in a namespace; the
  /// model's calls then name it, and the history repeats it.
  static let toolNamespace = "tsugumi"

  /// The conversation as Responses API input items: messages, function calls
  /// (the model's tool uses) and their outputs.
  static func responsesInput(_ messages: [ChatMessage], namespace: String? = nil) -> [[String: Any]] {
    var out: [[String: Any]] = []
    for m in messages {
      var texts: [[String: Any]] = []
      func flush() {
        if !texts.isEmpty { out.append(["role": m.role, "content": texts]); texts = [] }
      }
      for b in m.content {
        switch b {
        case .text(let t): texts.append(["type": m.role == "assistant" ? "output_text" : "input_text", "text": t])
        // In words: a web_search_call item would bring back none of its results.
        case .webSearch(let s): texts.append(["type": "output_text", "text": s.note])
        case .toolUse(let id, let name, let input):
          flush()
          var call: [String: Any] = ["type": "function_call", "call_id": id, "name": name, "arguments": ContentBlock.arguments(input) == nil ? "{}" : input]
          if let namespace { call["namespace"] = namespace }
          out.append(call)
        case .toolResult(let id, let content, _, let images):
          flush()
          let shown: [[String: Any]] = images.map { ["type": "input_image", "image_url": "data:\($0.mediaType);base64,\($0.data)"] }
          let sent: Any = images.isEmpty ? content : [["type": "input_text", "text": content]] + shown
          out.append(["type": "function_call_output", "call_id": id, "output": sent])
        }
      }
      flush()
    }
    return out
  }

  /// The request body. With the ChatGPT plan: no max_output_tokens (not
  /// accepted) and the tools in a namespace; store false and stream true always.
  func responsesBody(system: String, messages: [ChatMessage], tools: [ToolSpec]) -> [String: Any] {
    let plan = config.auth == .chatGPT
    var body: [String: Any] = [
      "model": config.model,
      "instructions": system,
      "input": Self.responsesInput(messages, namespace: plan ? Self.toolNamespace : nil),
      "stream": true,
      "store": false,
    ]
    if !plan { body["max_output_tokens"] = config.outputLimit }
    if let effort = config.effort { body["reasoning"] = ["effort": effort] }
    if config.fast { body["service_tier"] = "fast" }
    if !tools.isEmpty {
      let own = tools.filter { !$0.server }
      let functions: [[String: Any]] = own.map { ["type": "function", "name": $0.name, "description": $0.description, "parameters": $0.inputSchema] }
      var wire: [[String: Any]] = plan
        ? (functions.isEmpty ? [] : [["type": "namespace", "name": Self.toolNamespace, "description": "Tools of the open Tsugumi project.", "tools": functions]])
        : functions
      if own.count != tools.count {
        wire.append(["type": Shared.data.webSearch.openAIType])
        // The sources of a search (the plan's rules do not say it takes
        // `include`: there the cited sources are what can be followed).
        if !plan { body["include"] = [Shared.data.webSearch.openAIInclude] }
      }
      body["tools"] = wire
    }
    return body
  }

  /// A failure of a ChatGPT plan request, by its documented error code
  /// (siwc errors and recovery): the usage limit stops the turn; an
  /// unavailable usage check or user is retried like a 503.
  static func planError(status: Int, error: [String: Any]?, detail: String, requestId: String?) -> Error {
    let code = error?["code"] as? String
    let message = error?["message"] as? String ?? detail
    let rid = requestId.map { " (request \($0))" } ?? ""
    switch code {
    case "subscription_sharing_usage_limit_exceeded": return ChatGPTError.usageLimit
    case "subscription_sharing_usage_unavailable", "subscription_sharing_user_unavailable":
      return HTTPStatusError(status: 503, body: "\(code!): \(message)\(rid)")
    case "subscription_sharing_user_not_eligible": return ChatGPTError.notEligible(message)
    case "subscription_sharing_unsupported_capability":
      return ChatGPTError.unsupported((error?["param"] as? String).map { "\($0): \(message)" } ?? message)
    case let c?: return ChatGPTError.failed("\(c): \(message)\(rid)")
    case nil:
      switch status {
      case 401: return ChatGPTError.failed("the account or its permission was not accepted (401): \(message)\(rid)")
      case 403: return ChatGPTError.failed("blocked by a policy or permission check (403): \(message)\(rid)")
      default: return HTTPStatusError(status: status, body: message + rid)
      }
    }
  }

  func responsesStream(system: String, messages: [ChatMessage], tools: [ToolSpec]) throws -> AsyncThrowingStream<StreamEvent, Error> {
    let payload = try JSONSerialization.data(withJSONObject: responsesBody(system: system, messages: messages, tools: tools))
    let plan = config.auth == .chatGPT
    let session = self.session
    return AsyncThrowingStream { cont in
      let task = Task {
        do {
          var stale: String?
          var opened: (URLSession.AsyncBytes, URLResponse)?
          while opened == nil {
            var r = try await request(path: "v1/responses", stale: stale)
            r.httpMethod = "POST"
            r.httpBody = payload
            let (bytes, resp) = try await session.bytes(for: r)
            let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if status == 200 { opened = (bytes, resp); break }
            var text = ""
            for try await line in bytes.lines { text += line }
            // A refused access token is refreshed once (unless refreshed meanwhile).
            if plan && status == 401 && stale == nil {
              stale = r.value(forHTTPHeaderField: "authorization")?.replacingOccurrences(of: "Bearer ", with: "")
              continue
            }
            guard plan else { throw HTTPStatusError(status: status, body: text, retryAfter: Self.retryAfter(resp)) }
            let obj = (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? [String: Any]
            throw Self.planError(status: status, error: obj?["error"] as? [String: Any],
                                 detail: obj?["detail"] as? String ?? text,
                                 requestId: (resp as? HTTPURLResponse)?.value(forHTTPHeaderField: "x-request-id"))
          }
          let (bytes, resp) = opened!
          var usedTools = false
          var finished = false
          for try await line in bytes.lines {
            try Task.checkCancellation()
            guard line.hasPrefix("data:") else { continue }
            guard let obj = try? JSONSerialization.jsonObject(with: Data(line.dropFirst(5).utf8)) as? [String: Any] else { continue }
            let index = obj["output_index"] as? Int ?? 0
            switch obj["type"] as? String {
            case "response.output_text.delta":
              if let d = obj["delta"] as? String { cont.yield(.text(d)) }
            case "response.output_item.added":
              if let item = obj["item"] as? [String: Any], item["type"] as? String == "function_call" {
                usedTools = true
                var name = item["name"] as? String ?? ""
                if name.hasPrefix(Self.toolNamespace + ".") { name.removeFirst(Self.toolNamespace.count + 1) }
                cont.yield(.toolStart(index: index, id: item["call_id"] as? String ?? UUID().uuidString, name: name))
              }
            case "response.function_call_arguments.delta":
              if let d = obj["delta"] as? String { cont.yield(.toolInput(index: index, partialJSON: d)) }
            case "response.output_item.done":
              let item = obj["item"] as? [String: Any]
              switch item?["type"] as? String {
              case "function_call":
                cont.yield(.blockStop(index: index))
              case "web_search_call":
                let id = item?["id"] as? String ?? UUID().uuidString
                let (query, sources) = Self.openAISearch(item?["action"] as? [String: Any])
                cont.yield(.search(id: id, query: query))
                cont.yield(.searchResult(id: id, sources: sources, error: item?["status"] as? String == "failed" ? "failed" : nil))
              case "message":
                for c in item?["content"] as? [[String: Any]] ?? [] {
                  for a in c["annotations"] as? [[String: Any]] ?? [] where a["type"] as? String == "url_citation" {
                    if let url = a["url"] as? String { cont.yield(.citation(WebSource(url: url, title: a["title"] as? String))) }
                  }
                }
              default:
                break
              }
            case "response.completed", "response.incomplete":
              var usage = Usage()
              let u = (obj["response"] as? [String: Any])?["usage"] as? [String: Any]
              usage.inputTokens = u?["input_tokens"] as? Int ?? 0
              usage.outputTokens = u?["output_tokens"] as? Int ?? 0
              // OpenAI caches long prefixes by itself and reports what it read.
              usage.cacheReadTokens = (u?["input_tokens_details"] as? [String: Any])?["cached_tokens"] as? Int ?? 0
              let incomplete = obj["type"] as? String == "response.incomplete"
              cont.yield(.stop(reason: incomplete ? "max_tokens" : usedTools ? "tool_use" : "end_turn", usage: usage))
              finished = true
            case "response.failed", "error":
              let e = ((obj["response"] as? [String: Any])?["error"] ?? obj["error"]) as? [String: Any]
              if plan {
                throw Self.planError(status: 0, error: e, detail: "stream error",
                                     requestId: (resp as? HTTPURLResponse)?.value(forHTTPHeaderField: "x-request-id"))
              }
              throw AgentError.provider(e?["message"] as? String ?? e?["code"] as? String ?? "stream error")
            default:
              break
            }
          }
          if !finished { throw URLError(.networkConnectionLost) }
          cont.finish()
        } catch {
          cont.finish(throwing: error)
        }
      }
      cont.onTermination = { _ in task.cancel() }
    }
  }
}

// MARK: - Web search

/// A reply's blocks as the Messages API streams them. No thinking blocks: this
/// Agent sends none back anywhere, and one in an edited history is refused.
struct ReplyBlocks {
  private var blocks: [Int: [String: Any]] = [:]
  private var inputs: [Int: String] = [:]

  mutating func start(_ index: Int, _ block: [String: Any]) {
    if ["text", "tool_use", "server_tool_use", "web_search_tool_result"].contains(block["type"] as? String ?? "") { blocks[index] = block }
  }
  mutating func text(_ index: Int, _ t: String) {
    let so_far = blocks[index]?["text"] as? String ?? ""
    blocks[index]?["text"] = so_far + t
  }
  mutating func input(_ index: Int, _ json: String) { inputs[index, default: ""] += json }
  mutating func citation(_ index: Int, _ c: [String: Any]) {
    let so_far = blocks[index]?["citations"] as? [[String: Any]] ?? []
    blocks[index]?["citations"] = so_far + [c]
  }
  func isSearch(_ index: Int) -> Bool { blocks[index]?["type"] as? String == "server_tool_use" }

  /// The id and query of a server_tool_use block that has ended.
  func search(_ index: Int) -> (String, String)? {
    guard isSearch(index), let input = parsedInput(index) else { return nil }
    return (blocks[index]?["id"] as? String ?? UUID().uuidString, input["query"] as? String ?? "")
  }

  private func parsedInput(_ index: Int) -> [String: Any]? {
    (try? JSONSerialization.jsonObject(with: Data((inputs[index] ?? "{}").utf8))) as? [String: Any] ?? [:]
  }

  /// JSON text of the blocks, when the reply has anything only they can carry.
  var toSendBack: String? {
    var out: [[String: Any]] = []
    for index in blocks.keys.sorted() {
      var b = blocks[index]!
      let type = b["type"] as? String
      if type == "tool_use" || type == "server_tool_use" { b["input"] = parsedInput(index) ?? [:] }
      if type == "text" {
        if (b["text"] as? String ?? "").isEmpty { continue }
        if (b["citations"] as? [Any])?.isEmpty != false { b["citations"] = nil }
      }
      out.append(b)
    }
    let needed = out.contains { $0["type"] as? String == "server_tool_use" || $0["type"] as? String == "web_search_tool_result" || $0["citations"] != nil }
    guard needed, let data = try? JSONSerialization.data(withJSONObject: out) else { return nil }
    return String(decoding: data, as: UTF8.self)
  }
}

extension MessagesProvider {
  /// The basic tool: dynamic filtering (the later versions) runs code at the
  /// provider, which needs a container kept through the turn and is outside
  /// zero data retention. A turn's searches are capped.
  static var webSearchWire: [String: Any] {
    let w = Shared.data.webSearch
    return ["type": w.anthropicType, "name": w.name, "max_uses": w.maxUses]
  }

  static func providerBlocks(_ m: ChatMessage) -> [[String: Any]]? {
    m.providerContent.flatMap { (try? JSONSerialization.jsonObject(with: Data($0.utf8))) as? [[String: Any]] }
  }

  /// Whether a block may hold a cache breakpoint: not the provider's own blocks.
  static func cacheable(_ b: [String: Any]) -> Bool {
    switch b["type"] as? String {
    case "tool_use", "tool_result": return true
    case "text": return b["citations"] == nil
    default: return false
    }
  }

  /// The messages whose search blocks can go back to the API as they came. It
  /// refuses a search without its result, unless the conversation ends there
  /// (pause_turn, or one that waits for our tool results), and a result
  /// without its search: after a stopped turn or a compaction those go in words.
  static func replayable(_ messages: [ChatMessage]) -> Set<Int> {
    var keep = Set(messages.indices.filter { providerBlocks(messages[$0]) != nil })
    let last = messages.count - 1
    let waitingForResults = last >= 0 && messages[last].role == "user" && messages[last].content.allSatisfy {
      if case .toolResult = $0 { return true }
      return false
    }
    while true {
      var open: [String: Int] = [:]
      var bad: Int?
      for i in keep.sorted() {
        for b in providerBlocks(messages[i]) ?? [] {
          switch b["type"] as? String {
          case "server_tool_use": open[b["id"] as? String ?? ""] = i
          case "web_search_tool_result":
            if open.removeValue(forKey: b["tool_use_id"] as? String ?? "") == nil { bad = i }
          default: break
          }
        }
      }
      for i in open.values where i != last && !(i == last - 1 && waitingForResults) { bad = i }
      guard let bad else { return keep }
      keep.remove(bad)
    }
  }

  /// The sources in a web_search_tool_result's content, or its error object.
  static func searchResult(_ content: Any?) -> ([WebSource], String?) {
    guard let list = content as? [[String: Any]] else {
      return ([], (content as? [String: Any])?["error_code"] as? String ?? "unavailable")
    }
    return (list.compactMap { r in (r["url"] as? String).map { WebSource(url: $0, title: r["title"] as? String) } }, nil)
  }

  /// What a web_search_call did: a search, or a look at a page.
  static func openAISearch(_ action: [String: Any]?) -> (query: String, sources: [WebSource]) {
    let url = action?["url"] as? String
    var sources = (action?["sources"] as? [[String: Any]] ?? []).compactMap { ($0["url"] as? String).map { WebSource(url: $0) } }
    if let url { sources.append(WebSource(url: url)) }
    let queries = action?["queries"] as? [String] ?? (action?["query"] as? String).map { [$0] } ?? []
    let query = queries.isEmpty ? (action?["pattern"] as? String ?? url ?? "") : queries.joined(separator: ", ")
    return (query, sources)
  }
}
