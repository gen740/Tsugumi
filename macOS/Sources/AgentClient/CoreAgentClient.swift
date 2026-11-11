import Darwin
import Foundation

public struct AgentProject: Sendable {
  public let id: String
  public let endpoint: String
  public let token: String
  public let title: String
  public init(id: String, endpoint: String, token: String, title: String) {
    self.id = id
    self.endpoint = endpoint
    self.token = token
    self.title = title
  }
}

public struct CoreAgentFailure: Error, CustomStringConvertible, Sendable {
  public let code: String
  public let message: String
  public let requestID: String?
  public var description: String { message }
}

/// The native C++ client is a persistent child process. It owns protocol
/// validation, request framing and socket I/O; Swift only adapts UI values.
public actor CoreAgentClient {
  public static let shared = CoreAgentClient()
  private var process: Process?
  private var connectionID = UUID()
  private var input: FileHandle?
  private var reader: Task<Void, Never>?
  private var opening: Task<Void, Error>?
  private var buffer = Data()
  private var nextID: UInt64 = 0
  private var pending: [UInt64: CheckedContinuation<[String: Any], Error>] = [:]
  private var timeouts: [UInt64: Task<Void, Never>] = [:]

  private var stateDirectory: URL {
    if let path = ProcessInfo.processInfo.environment["TSUGUMI_AGENT_STATE"] { return URL(fileURLWithPath: path) }
    let folder = Bundle.main.object(forInfoDictionaryKey: "TsugumiChannel") as? String == "release" ? "Tsugumi" : "Tsugumi-dev"
    let support = ProcessInfo.processInfo.environment["TSUGUMI_SUPPORT_DIR"].map { URL(fileURLWithPath: $0) }
      ?? FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/" + folder)
    return support.appendingPathComponent("agent-core")
  }

  private static var repository: URL {
    #if DEBUG
    return URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../..", isDirectory: true).standardizedFileURL
    #else
    return Bundle.main.bundleURL
    #endif
  }

  private func executable(_ name: String, environment: String, component: String) throws -> URL {
    if let p = ProcessInfo.processInfo.environment[environment] { return URL(fileURLWithPath: p) }
    if let bundled = Bundle.main.url(forAuxiliaryExecutable: name) { return bundled }
    let build = ProcessInfo.processInfo.environment["TSUGUMI_BUILD_DIR"].map { URL(fileURLWithPath: $0) }
      ?? Self.repository.appendingPathComponent("build")
    let built = build.appendingPathComponent(component).appendingPathComponent(name)
    guard FileManager.default.isExecutableFile(atPath: built.path) else {
      throw CoreAgentFailure(code: "unavailable", message: "The core Agent executable is missing: \(name)", requestID: nil)
    }
    return built
  }

  private func ready() async throws -> [String: Any] {
    let state = stateDirectory
    let file = state.appendingPathComponent("agent.ready")
    func read() -> [String: Any]? {
      guard let bytes = try? Data(contentsOf: file),
            let object = try? JSONSerialization.jsonObject(with: bytes) as? [String: Any],
            let pid = object["pid"] as? Int32, kill(pid, 0) == 0,
            object["protocol"] as? Int == 1, object["unix_socket"] is String, object["user_token"] is String else { return nil }
      return object
    }
    if let existing = read() { return existing }
    try FileManager.default.createDirectory(at: state, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    let directory = URL(fileURLWithPath: "/private/tmp", isDirectory: true).appendingPathComponent("tsugumi-agent-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    let daemon = Process()
    daemon.executableURL = try executable("tsugumi-agentd", environment: "TSUGUMI_AGENTD", component: "core")
    let demos = (Bundle.main.resourceURL?.appendingPathComponent("demos") ?? Self.repository.appendingPathComponent("demos"))
      .appendingPathComponent(Bundle.main.preferredLocalizations.first?.hasPrefix("en") == true ? "en" : "ja")
    daemon.arguments = ["--state-dir", state.path, "--unix", directory.appendingPathComponent("agent.sock").path,
                        "--listen", "127.0.0.1:0", "--demos-dir", demos.path]
    let log = state.appendingPathComponent("agent.log")
    if !FileManager.default.fileExists(atPath: log.path) { FileManager.default.createFile(atPath: log.path, contents: nil, attributes: [.posixPermissions: 0o600]) }
    let handle = try FileHandle(forWritingTo: log)
    try handle.seekToEnd()
    daemon.standardOutput = handle
    daemon.standardError = handle
    daemon.standardInput = FileHandle.nullDevice
    try daemon.run()
    try handle.close()
    let deadline = Date().addingTimeInterval(15)
    while Date() < deadline {
      if let value = read() { return value }
      if !daemon.isRunning { throw CoreAgentFailure(code: "unavailable", message: "The core Agent exited during startup; see \(log.path)", requestID: nil) }
      try await Task.sleep(for: .milliseconds(50))
    }
    throw CoreAgentFailure(code: "timeout", message: "The core Agent did not become ready", requestID: nil)
  }

  private func connect() async throws {
    if let opening { return try await opening.value }
    if process != nil { return }
    let task = Task { try await self.start() }
    opening = task
    do { try await task.value; opening = nil } catch { opening = nil; throw error }
  }

  private func start() async throws {
    let ready = try await ready()
    guard let socket = ready["unix_socket"] as? String, let token = ready["user_token"] as? String else {
      throw CoreAgentFailure(code: "protocol", message: "Invalid Agent endpoint", requestID: nil)
    }
    let schema = ProcessInfo.processInfo.environment["TSUGUMI_AGENT_SCHEMA"].map { URL(fileURLWithPath: $0) }
      ?? Bundle.main.url(forResource: "agent", withExtension: "bfbs")
      ?? Self.repository.appendingPathComponent("build/generated/bfbs/agent.bfbs")
    let identity = UUID()
    connectionID = identity
    let child = Process()
    child.executableURL = try executable("tsugumi-rpc", environment: "TSUGUMI_RPC_CLIENT", component: "client")
    child.arguments = ["--endpoint", "unix:" + socket, "--schema", schema.path, "--subprotocol", "tsugumi.agent.v1"]
    let incoming = Pipe(), outgoing = Pipe()
    child.standardInput = incoming
    child.standardOutput = outgoing
    child.standardError = FileHandle.standardError
    child.terminationHandler = { [weak self] _ in Task { await self?.closed(identity) } }
    try child.run()
    process = child
    input = incoming.fileHandleForWriting
    let output = outgoing.fileHandleForReading
    reader = Task.detached { [weak self] in
      while !Task.isCancelled {
        let data = output.availableData
        if data.isEmpty { break }
        await self?.received(data, identity)
      }
      await self?.closed(identity)
    }
    _ = try await write("HelloRequest", ["token": token, "client_id": "macos-\(UUID().uuidString)", "protocol": 1])
  }

  private func closed(_ identity: UUID) {
    guard identity == connectionID else { return }
    process = nil
    input = nil
    buffer.removeAll()
    for continuation in pending.values { continuation.resume(throwing: CoreAgentFailure(code: "disconnected", message: "The core Agent connection closed", requestID: nil)) }
    pending.removeAll()
    for timeout in timeouts.values { timeout.cancel() }
    timeouts.removeAll()
  }

  private func received(_ data: Data, _ identity: UUID) {
    guard identity == connectionID else { return }
    buffer.append(data)
    while let newline = buffer.firstIndex(of: 10) {
      let line = buffer[..<newline]
      buffer.removeSubrange(...newline)
      guard let message = try? JSONSerialization.jsonObject(with: line) as? [String: Any] else { continue }
      if message["transport_error"] != nil { process?.terminate(); closed(identity); return }
      guard let id = message["request_id"] as? UInt64,
            let continuation = pending.removeValue(forKey: id) else { continue }
      timeouts.removeValue(forKey: id)?.cancel()
      guard message["payload_type"] as? String == "Reply", let payload = message["payload"] as? [String: Any],
            let value = payload["value"] as? [String: Any] else {
        continuation.resume(throwing: CoreAgentFailure(code: "protocol", message: "Invalid Agent reply", requestID: nil)); continue
      }
      if payload["value_type"] as? String == "Failure" {
        continuation.resume(throwing: CoreAgentFailure(code: value["code"] as? String ?? "error",
          message: value["message"] as? String ?? "Agent request failed", requestID: value["request_id"] as? String))
      } else if payload["value_type"] as? String == "Result", let json = value["json"] as? String,
                let result = try? JSONSerialization.jsonObject(with: Data(json.utf8), options: [.fragmentsAllowed]) {
        continuation.resume(returning: result as? [String: Any] ?? ["value": result])
      } else { continuation.resume(returning: value) }
    }
    if buffer.count > 32 << 20 { process?.terminate(); closed(identity) }
  }

  private func expired(_ id: UInt64) {
    timeouts.removeValue(forKey: id)
    pending.removeValue(forKey: id)?.resume(throwing: CoreAgentFailure(code: "timeout", message: "The core Agent did not answer", requestID: nil))
  }

  private func write(_ method: String, _ fields: [String: Any]) async throws -> [String: Any] {
    guard let input else { throw CoreAgentFailure(code: "disconnected", message: "The Agent connection is closed", requestID: nil) }
    nextID += 1
    let id = nextID
    // FlatBuffers' JSON reader requires a union discriminator before its value.
    // Dictionary serialization does not preserve that ordering across processes.
    let methodJSON = String(decoding: try JSONEncoder().encode(method), as: UTF8.self)
    var data = Data("{\"request_id\":\(id),\"payload_type\":\"Request\",\"payload\":{\"value_type\":\(methodJSON),\"value\":".utf8)
    data.append(try JSONSerialization.data(withJSONObject: fields))
    data.append(Data("}}\n".utf8))
    return try await withCheckedThrowingContinuation { continuation in
      pending[id] = continuation
      timeouts[id] = Task { try? await Task.sleep(for: .seconds(120)); if !Task.isCancelled { self.expired(id) } }
      do { try input.write(contentsOf: data) }
      catch { pending.removeValue(forKey: id)?.resume(throwing: error); timeouts.removeValue(forKey: id)?.cancel() }
    }
  }

  public func call(_ method: String, _ fields: [String: Any] = [:]) async throws -> [String: Any] {
    try await connect()
    return try await write(method, fields)
  }

  public func configure(_ config: ProviderConfig, conversationID: String? = nil, asking: Bool = false) async throws {
    var fields: [String: Any] = ["provider_json": try await providerJSON(config, asking: asking)]
    _ = try await call("ConfigureRequest", fields)
    if let conversationID {
      fields["conversation_id"] = conversationID
      _ = try await call("ConfigureRequest", fields)
    }
  }

  public func providerJSON(_ config: ProviderConfig, asking: Bool = false) async throws -> String {
    var object = try JSONSerialization.jsonObject(with: JSONEncoder().encode(config)) as! [String: Any]
    object["auth"] = config.auth == .chatGPT ? "chatgpt" : config.auth.rawValue
    switch config.auth {
    case .none: break
    case .apiKey:
      guard let key = asking ? Keychain.load(account: config.name) : Keychain.loadWithoutAsking(account: config.name) else {
        throw CoreAgentFailure(code: "credentials", message: "No API key for \(config.name)", requestID: nil)
      }
      object["apiKey"] = key
    case .chatGPT:
      let token = try await config.chatGPTAuth.accessToken(asking: asking)
      object["token"] = token.value
      object["accountId"] = token.clientId
      object["credential_id"] = token.clientId
    }
    return String(decoding: try JSONSerialization.data(withJSONObject: object), as: UTF8.self)
  }

  public func models(_ config: ProviderConfig, asking: Bool = false) async throws -> [(id: String, label: String)] {
    let result = try await call("ModelsListRequest", ["provider_json": try await providerJSON(config, asking: asking)])
    return (result["value"] as? [[String: Any]] ?? []).compactMap { item in
      guard let id = item["id"] as? String else { return nil }
      return (id, item["label"] as? String ?? id)
    }
  }
}
