import Darwin
import Foundation
import TsugumiRPC

public struct AgentProject: Sendable {
  public let id: String
  public let endpoint: String
  public let token: String
  public let title: String
  public let path: String
  public init(id: String, endpoint: String, token: String, title: String, path: String = "") {
    self.id = id
    self.endpoint = endpoint
    self.token = token
    self.title = title
    self.path = path
  }
}

public struct CoreAgentFailure: LocalizedError, CustomStringConvertible, Sendable {
  public let code: String
  public let message: String
  public let requestID: String?
  public var description: String { message }
  public var errorDescription: String? { message }
}

/// A presentation snapshot of agentd's catalogs. Persistence and refresh policy
/// belong to agentd; this lets an open native menu draw without an actor hop.
public final class ModelCatalogSnapshot: @unchecked Sendable {
  public static let shared = ModelCatalogSnapshot()
  private let lock = NSLock()
  private var catalogs: [String: [(id: String, name: String)]] = [:]
  public func models(_ id: String) -> [(id: String, name: String)] {
    lock.lock(); defer { lock.unlock() }
    return catalogs[id] ?? []
  }
  fileprivate func show(_ id: String, _ models: [(id: String, name: String)]) {
    lock.lock(); defer { lock.unlock() }
    catalogs[id] = models
  }
}

/// Binary Agent and Host connections share the project's C ABI byte transport.
/// Framing, verification, deadlines and pending requests belong to client/.
public actor CoreAgentClient {
  public static let shared = CoreAgentClient()
  private let supportOverride: URL?
  private let keyProfile = "macos:keychain:" + UUID().uuidString

  public init(support: URL? = nil) { supportOverride = support }
  private var transports: [Bool: NativeByteTransport] = [:]
  private var connectionID = UUID()
  private var opening: Task<Void, Error>?
  private var nextID: UInt64 = 0

  private var stateDirectory: URL {
    if let path = ProcessInfo.processInfo.environment["TSUGUMI_AGENT_STATE"] { return URL(fileURLWithPath: path) }
    if let supportOverride { return supportOverride.appendingPathComponent("agent-core") }
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
            object["protocol"] as? Int == 1, object["unix_socket"] is String, object["host_socket"] is String, object["user_token"] is String else { return nil }
      return object
    }
    if let existing = read() { return existing }
    try FileManager.default.createDirectory(at: state, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    let directory = URL(fileURLWithPath: "/private/tmp", isDirectory: true).appendingPathComponent("tsugumi-agent-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    let daemon = Process()
    daemon.executableURL = try executable("tsugumi-agentd", environment: "TSUGUMI_AGENTD", component: "core")
    let demos = Bundle.main.url(forResource: "demos", withExtension: nil) ?? Self.repository.appendingPathComponent("demos")
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
    if !transports.isEmpty { return }
    let task = Task { try await self.start() }
    opening = task
    do { try await task.value; opening = nil }
    catch { opening = nil; close(); throw error }
  }

  private func start() async throws {
    let ready = try await ready()
    guard let agentSocket = ready["unix_socket"] as? String, let hostSocket = ready["host_socket"] as? String,
          let token = ready["user_token"] as? String else {
      throw CoreAgentFailure(code: "protocol", message: "Invalid service endpoints", requestID: nil)
    }
    let identity = UUID()
    connectionID = identity
    for host in [false, true] {
      let transport = NativeByteTransport(endpoint: "unix:" + (host ? hostSocket : agentSocket), contract: host ? .host : .agent)
      transports[host] = transport
      transport.observe(receive: { _ in }, closed: { [weak self] in
        Task { await self?.closed(identity) }
      })
      try await transport.start()
      _ = try await exchange("HelloRequest", ["token": token, "client_id": "macos-\(UUID().uuidString)", "protocol": 1], host: host)
    }
  }

  private func close() {
    connectionID = UUID()
    let previous = transports
    transports.removeAll()
    for transport in previous.values { transport.close() }
  }

  private func closed(_ identity: UUID) {
    guard identity == connectionID else { return }
    close()
  }

  private func exchange(_ method: String, _ fields: [String: Any], host: Bool) async throws -> [String: Any] {
    guard let transport = transports[host], nextID < UInt64.max else {
      throw CoreAgentFailure(code: "disconnected", message: "The service connection is closed", requestID: nil)
    }
    nextID += 1
    let frame = try ServiceWire.encode(method, fields: fields, id: nextID, host: host)
    let data: Data = try await withCheckedThrowingContinuation { continuation in
      transport.call(frame, timeout: .seconds(120), continuation: continuation)
    }
    return try ServiceWire.decode(data, host: host)
  }

  public func call(_ method: String, _ fields: [String: Any] = [:]) async throws -> [String: Any] {
    try await connect()
    return try await exchange(method, fields, host: ServiceWire.hostMethod(method, fields))
  }

  public func configure(_ config: ProviderConfig, conversationID: String) async throws {
    _ = try await call("ConfigureRequest", ["provider": try await provider(config), "conversation_id": conversationID])
  }

  public func provider(_ config: ProviderConfig, asking: Bool = true) async throws -> Tsugumi_Service_ProviderSelectionT {
    let value = Tsugumi_Service_ProviderSelectionT()
    value.model = config.model
    value.useChoices = config.usesChoices
    value.profile = "desktop"
    value.providerId = config.connection
    value.accountProfile = "desktop"
    if config.auth == .apiKey {
      guard let key = asking ? Keychain.load(account: config.name) : Keychain.loadWithoutAsking(account: config.name) else {
        throw CoreAgentFailure(code: "credentials", message: "No API key for \(config.name)", requestID: nil)
      }
      try await provisionKey(key, id: config.connection)
      value.keyProfile = keyProfile
    }
    return value
  }

  public func provisionKey(_ key: String?, id: String) async throws {
    var fields: [String: Any] = ["profile": keyProfile, "id": id, "ephemeral": true, "remove": key == nil]
    if let key { let input = Tsugumi_Service_ProviderInputT(); input.apiKey = key; fields["provider"] = input }
    _ = try await call("ProviderSetRequest", fields)
  }

  public func storedModels(_ id: String) async throws -> [(id: String, name: String)] {
    try await modelList(id: id)
  }

  private func modelList(id: String, refresh: Bool = false, keyProfile: String? = nil) async throws -> [(id: String, name: String)] {
    var fields: [String: Any] = ["provider_id": id, "refresh": refresh, "profile": "desktop"]
    if let keyProfile { fields["key_profile"] = keyProfile }
    let result = try await call("ModelsListRequest", fields)
    let list: [(id: String, name: String)] = (result["value"] as? [[String: Any]] ?? []).compactMap { item in
      guard let id = item["id"] as? String else { return nil }
      return (id, item["name"] as? String ?? id)
    }
    ModelCatalogSnapshot.shared.show(id, list)
    return list
  }

  public func models(_ config: ProviderConfig, asking: Bool = false) async throws -> [(id: String, name: String)] {
    let selection = try await provider(config, asking: asking)
    return try await modelList(id: config.connection, refresh: true, keyProfile: selection.keyProfile)
  }
}
