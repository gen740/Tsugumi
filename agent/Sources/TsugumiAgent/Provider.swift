import Foundation
import Security

/// One content block of a chat message (Anthropic Messages wire shape).
public enum ContentBlock: Codable, Equatable {
  case text(String)
  case toolUse(id: String, name: String, input: String)  // input: JSON text
  case toolResult(toolUseId: String, content: String, isError: Bool)

  enum CodingKeys: String, CodingKey { case type, text, id, name, input, tool_use_id, content, is_error }

  public init(from d: Decoder) throws {
    let c = try d.container(keyedBy: CodingKeys.self)
    switch try c.decode(String.self, forKey: .type) {
    case "text": self = .text(try c.decode(String.self, forKey: .text))
    case "tool_use": self = .toolUse(id: try c.decode(String.self, forKey: .id), name: try c.decode(String.self, forKey: .name),
                                     input: try c.decode(String.self, forKey: .input))
    default: self = .toolResult(toolUseId: try c.decode(String.self, forKey: .tool_use_id),
                                content: try c.decode(String.self, forKey: .content),
                                isError: try c.decode(Bool.self, forKey: .is_error))
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
    case .toolResult(let id, let content, let isError):
      try c.encode("tool_result", forKey: .type)
      try c.encode(id, forKey: .tool_use_id)
      try c.encode(content, forKey: .content)
      try c.encode(isError, forKey: .is_error)
    }
  }

  /// Wire JSON for the Messages API.
  var wire: [String: Any] {
    switch self {
    case .text(let t): return ["type": "text", "text": t]
    case .toolUse(let id, let name, let input):
      let obj = (try? JSONSerialization.jsonObject(with: Data(input.utf8))) ?? [String: Any]()
      return ["type": "tool_use", "id": id, "name": name, "input": obj]
    case .toolResult(let id, let content, let isError):
      return ["type": "tool_result", "tool_use_id": id, "content": content, "is_error": isError]
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

public enum StreamEvent {
  case text(String)
  case toolStart(index: Int, id: String, name: String)
  case toolInput(index: Int, partialJSON: String)
  case blockStop(index: Int)
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

  public init(name: String, baseURL: URL, model: String, auth: Auth, api: API = .anthropicMessages) {
    self.name = name
    self.baseURL = baseURL
    self.model = model
    self.auth = auth
    self.api = api
  }

  /// A real provider (not the local fixture).
  public var isRemote: Bool { auth != .none }

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
  /// with a cache breakpoint (at most 4 are allowed).
  static func cachedBody(system: String, messages: [ChatMessage], tools: [ToolSpec]) -> (system: Any, messages: Any, tools: Any) {
    let cache: [String: Any] = ["type": "ephemeral"]
    var wireTools: [[String: Any]] = tools.map { ["name": $0.name, "description": $0.description, "input_schema": $0.inputSchema] }
    if !wireTools.isEmpty { wireTools[wireTools.count - 1]["cache_control"] = cache }
    var wire: [[String: Any]] = messages.map { ["role": $0.role, "content": $0.content.map(\.wire)] }
    if var last = wire.last, var content = last["content"] as? [[String: Any]], !content.isEmpty {
      content[content.count - 1]["cache_control"] = cache
      last["content"] = content
      wire[wire.count - 1] = last
    }
    return ([["type": "text", "text": system, "cache_control": cache]], wire, wireTools)
  }

  public func stream(system: String, messages: [ChatMessage], tools: [ToolSpec]) throws -> AsyncThrowingStream<StreamEvent, Error> {
    if config.api == .openAIResponses { return try responsesStream(system: system, messages: messages, tools: tools) }
    let cached = Self.cachedBody(system: system, messages: messages, tools: tools)
    let body: [String: Any] = [
      "model": config.model,
      "max_tokens": config.maxTokens,
      "stream": true,
      "system": cached.system,
      "messages": cached.messages,
      "tools": cached.tools,
    ]
    let payload = try JSONSerialization.data(withJSONObject: body)
    let session = self.session
    return AsyncThrowingStream { cont in
      let task = Task {
        do {
          var r = try await request(path: "v1/messages")
          r.httpMethod = "POST"
          r.httpBody = payload
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
              if let b = obj["content_block"] as? [String: Any], b["type"] as? String == "tool_use" {
                cont.yield(.toolStart(index: idx, id: b["id"] as? String ?? UUID().uuidString, name: b["name"] as? String ?? ""))
              }
            case "content_block_delta":
              let idx = obj["index"] as? Int ?? 0
              if let d = obj["delta"] as? [String: Any] {
                if let t = d["text"] as? String { cont.yield(.text(t)) }
                if let pj = d["partial_json"] as? String { cont.yield(.toolInput(index: idx, partialJSON: pj)) }
              }
            case "content_block_stop":
              cont.yield(.blockStop(index: obj["index"] as? Int ?? 0))
            case "message_stop":
              stopped = true
            case "message_delta":
              if let u = obj["usage"] as? [String: Any] { usage.outputTokens = u["output_tokens"] as? Int ?? usage.outputTokens }
              if let d = obj["delta"] as? [String: Any], let reason = d["stop_reason"] as? String {
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
        case .toolUse(let id, let name, let input):
          flush()
          var call: [String: Any] = ["type": "function_call", "call_id": id, "name": name, "arguments": input]
          if let namespace { call["namespace"] = namespace }
          out.append(call)
        case .toolResult(let id, let content, _):
          flush()
          out.append(["type": "function_call_output", "call_id": id, "output": content])
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
    if !plan { body["max_output_tokens"] = config.maxTokens }
    if !tools.isEmpty {
      let functions: [[String: Any]] = tools.map { ["type": "function", "name": $0.name, "description": $0.description, "parameters": $0.inputSchema] }
      body["tools"] = plan
        ? [["type": "namespace", "name": Self.toolNamespace, "description": "Tools of the open Tsugumi project.", "tools": functions]]
        : functions
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
              if (obj["item"] as? [String: Any])?["type"] as? String == "function_call" { cont.yield(.blockStop(index: index)) }
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
