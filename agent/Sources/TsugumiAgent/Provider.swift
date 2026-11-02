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
  public init(role: String, content: [ContentBlock]) {
    self.role = role
    self.content = content
  }
}

public struct Usage: Codable, Equatable {
  public var inputTokens = 0
  public var outputTokens = 0
  public init() {}
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

  public var name: String
  public var baseURL: URL
  public var model: String
  public var usesKeychain: Bool  // false for the local fixture
  public var api: API = .anthropicMessages
  public var maxTokens = 4096
  public var contextLimit = 180_000

  public init(name: String, baseURL: URL, model: String, usesKeychain: Bool, api: API = .anthropicMessages) {
    self.name = name
    self.baseURL = baseURL
    self.model = model
    self.usesKeychain = usesKeychain
    self.api = api
  }

  public static let anthropic = ProviderConfig(name: "Anthropic", baseURL: URL(string: "https://api.anthropic.com")!,
                                               model: "claude-sonnet-5-5", usesKeychain: true)
  /// OpenAI with an API key (Responses API). Not a ChatGPT account sign-in.
  public static let openAI = ProviderConfig(name: "OpenAI", baseURL: URL(string: "https://api.openai.com")!,
                                            model: "gpt-6.1-sol", usesKeychain: true, api: .openAIResponses)
  public static func localFixture(port: Int) -> ProviderConfig {
    var c = ProviderConfig(name: "Local fixture", baseURL: URL(string: "http://127.0.0.1:\(port)")!, model: "fixture",
                           usesKeychain: false)
    c.contextLimit = 6000
    return c
  }
}

/// Model credentials stay in the macOS Keychain on the host (one item per
/// provider, readable only while the Mac is unlocked, not synced). They are
/// never written to project files, settings or logs, and never sent to
/// tsugumid or to Card workers.
public enum Keychain {
  static let service = "app.tsugumi.agent"

  /// Removes the stored secret of a provider (no error when there is none).
  public static func delete(account: String) {
    let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                            kSecAttrAccount as String: account]
    SecItemDelete(q as CFDictionary)
  }

  public static func save(_ secret: String, account: String) throws {
    let base: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                               kSecAttrAccount as String: account]
    SecItemDelete(base as CFDictionary)
    var add = base
    add[kSecValueData as String] = Data(secret.utf8)
    add[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
    let s = SecItemAdd(add as CFDictionary, nil)
    guard s == errSecSuccess else { throw AgentError.provider("Keychain error \(s)") }
  }

  public static func load(account: String) -> String? {
    let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                            kSecAttrAccount as String: account, kSecReturnData as String: true]
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

  public init(config: ProviderConfig) {
    self.config = config
    let c = URLSessionConfiguration.ephemeral
    c.timeoutIntervalForRequest = 120
    session = URLSession(configuration: c)
  }

  private func request(path: String) throws -> URLRequest {
    var r = URLRequest(url: config.baseURL.appendingPathComponent(path))
    r.setValue("application/json", forHTTPHeaderField: "content-type")
    if config.api == .anthropicMessages { r.setValue("2023-06-01", forHTTPHeaderField: "anthropic-version") }
    if config.usesKeychain {
      guard let key = Keychain.load(account: config.name) else {
        throw AgentError.provider("no API key for \(config.name); add it in Settings")
      }
      switch config.api {
      case .anthropicMessages: r.setValue(key, forHTTPHeaderField: "x-api-key")
      case .openAIResponses: r.setValue("Bearer " + key, forHTTPHeaderField: "authorization")
      }
    }
    return r
  }

  /// Model capability discovery (available models).
  public func models() async throws -> [String] {
    let (data, resp) = try await session.data(for: try request(path: "v1/models"))
    guard (resp as? HTTPURLResponse)?.statusCode == 200,
          let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any],
          let list = obj["data"] as? [[String: Any]] else {
      throw AgentError.provider("cannot list models")
    }
    return list.compactMap { $0["id"] as? String }
  }

  public struct HTTPStatusError: Error {
    public let status: Int
    public let body: String
    public var retryable: Bool { status == 429 || status >= 500 }
  }

  public func stream(system: String, messages: [ChatMessage], tools: [ToolSpec]) throws -> AsyncThrowingStream<StreamEvent, Error> {
    if config.api == .openAIResponses { return try responsesStream(system: system, messages: messages, tools: tools) }
    var r = try request(path: "v1/messages")
    r.httpMethod = "POST"
    let body: [String: Any] = [
      "model": config.model,
      "max_tokens": config.maxTokens,
      "stream": true,
      "system": system,
      "messages": messages.map { ["role": $0.role, "content": $0.content.map(\.wire)] },
      "tools": tools.map { ["name": $0.name, "description": $0.description, "input_schema": $0.inputSchema] },
    ]
    r.httpBody = try JSONSerialization.data(withJSONObject: body)
    let session = self.session
    return AsyncThrowingStream { cont in
      let task = Task {
        do {
          let (bytes, resp) = try await session.bytes(for: r)
          let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
          if status != 200 {
            var text = ""
            for try await line in bytes.lines { text += line }
            throw HTTPStatusError(status: status, body: text)
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
                usage.inputTokens = u["input_tokens"] as? Int ?? 0
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
  /// The conversation as Responses API input items: messages, function calls
  /// (the model's tool uses) and their outputs.
  static func responsesInput(_ messages: [ChatMessage]) -> [[String: Any]] {
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
          out.append(["type": "function_call", "call_id": id, "name": name, "arguments": input])
        case .toolResult(let id, let content, _):
          flush()
          out.append(["type": "function_call_output", "call_id": id, "output": content])
        }
      }
      flush()
    }
    return out
  }

  func responsesStream(system: String, messages: [ChatMessage], tools: [ToolSpec]) throws -> AsyncThrowingStream<StreamEvent, Error> {
    var r = try request(path: "v1/responses")
    r.httpMethod = "POST"
    var body: [String: Any] = [
      "model": config.model,
      "instructions": system,
      "input": Self.responsesInput(messages),
      "max_output_tokens": config.maxTokens,
      "stream": true,
      "store": false,
    ]
    if !tools.isEmpty {
      body["tools"] = tools.map { ["type": "function", "name": $0.name, "description": $0.description, "parameters": $0.inputSchema] }
    }
    r.httpBody = try JSONSerialization.data(withJSONObject: body)
    let session = self.session
    return AsyncThrowingStream { cont in
      let task = Task {
        do {
          let (bytes, resp) = try await session.bytes(for: r)
          let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
          if status != 200 {
            var text = ""
            for try await line in bytes.lines { text += line }
            throw HTTPStatusError(status: status, body: text)
          }
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
                cont.yield(.toolStart(index: index, id: item["call_id"] as? String ?? UUID().uuidString, name: item["name"] as? String ?? ""))
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
              let incomplete = obj["type"] as? String == "response.incomplete"
              cont.yield(.stop(reason: usedTools ? "tool_use" : incomplete ? "max_tokens" : "end_turn", usage: usage))
              finished = true
            case "response.failed", "error":
              let e = ((obj["response"] as? [String: Any])?["error"] ?? obj["error"]) as? [String: Any]
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
