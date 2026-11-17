import FlatBuffers
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

public struct CoreHostFailure: LocalizedError, CustomStringConvertible, Sendable {
  public let code: String
  public let message: String
  public let requestID: String?
  public var description: String {
    switch code {
    case "stale_project": return String(localized: "このフォルダのプロジェクト ID が変更または削除されました。このプロジェクトを閉じて開き直し、現在の ID で登録してください。会話の記録は削除していません。")
    case "conversation_identity_mismatch": return String(localized: "このプロジェクトの会話記録は別のプロジェクト ID に属しています。バックアップから対応する .tsugumi フォルダを復元してください。新しい会話を始める場合は、プロジェクトを閉じ、agent.sqlite を .tsugumi の外に移動して保管してから開き直してください。会話の記録は削除していません。")
    default: return message
    }
  }
  public var errorDescription: String? { description }
}

/// A presentation snapshot of hostd's catalogs. Persistence and refresh policy
/// belong to hostd; this lets an open native menu draw without an actor hop.
public final class ModelCatalogSnapshot: @unchecked Sendable {
  public static let shared = ModelCatalogSnapshot()
  private let lock = NSLock()
  private var availability: [String: Bool] = [:]
  public func usable(_ id: String) -> Bool {
    lock.lock(); defer { lock.unlock() }
    return availability[id] ?? false
  }
  fileprivate func showAvailability(_ id: String, _ usable: Bool) {
    lock.lock(); defer { lock.unlock() }; availability[id] = usable
  }
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

// Read observers normally reconnect across host restarts. Once this process
// starts an update/exit, none of its client actors may spawn the old host again.
private final class HostProcessLifetime: @unchecked Sendable {
  static let shared = HostProcessLifetime()
  private let lock = NSLock()
  private var ending = false
  private var starting: Set<Int32> = []
  private var signalled: Set<Int32> = []
  var isEnding: Bool { lock.withLock { ending } }
  func launched(_ pid: Int32) {
    let stopping = lock.withLock { starting.insert(pid); return ending }
    if stopping { requestStop(pid) }
  }
  func becameReady(_ pid: Int32) { _ = lock.withLock { starting.remove(pid) } }
  func end() {
    let pending = lock.withLock { ending = true; return starting }
    for pid in pending { requestStop(pid) }
  }
  func requestStop(_ pid: Int32) {
    // A second TERM forces hostd out. A launch completing during Quit must
    // share the same single signal as stopResident, preserving guest shutdown.
    if lock.withLock({ signalled.insert(pid).inserted }) { _ = kill(pid, SIGTERM) }
  }
}

/// Binary Agent and Host connections share the project's C ABI byte transport.
/// Framing, verification, deadlines and pending requests belong to client/.
public actor CoreHostClient {
  public static let shared = CoreHostClient()
  private let supportOverride: URL?
  private let keyProfile = "macos:keychain:" + UUID().uuidString

  public init(support: URL? = nil) { supportOverride = support }
  private var transports: [Bool: NativeByteTransport] = [:]
  private var connectionID = UUID()
  private var opening: Task<Void, Error>?
  private var nextID: UInt64 = 0
  private var runtimeArguments: [String]?
  private var hostObservers: [UUID: @Sendable ([String: Any]) -> Void] = [:]
  public func configureRuntime(_ arguments: [String]) { runtimeArguments = arguments }
  public func observeHost(_ handler: @escaping @Sendable ([String: Any]) -> Void) -> UUID {
    let token = UUID(); hostObservers[token] = handler; return token
  }
  private func hostEvent(_ data: Data, identity: UUID) {
    guard connectionID == identity, let event = try? ServiceWire.decodeEvent(data) else { return }
    for handler in hostObservers.values { handler(event) }
  }

  private var stateDirectory: URL {
    if let path = ProcessInfo.processInfo.environment["TSUGUMI_HOST_STATE"] { return URL(fileURLWithPath: path) }
    if let supportOverride { return supportOverride.appendingPathComponent("host-core") }
    let folder = Bundle.main.object(forInfoDictionaryKey: "TsugumiChannel") as? String == "release" ? "Tsugumi" : "Tsugumi-dev"
    let support = ProcessInfo.processInfo.environment["TSUGUMI_SUPPORT_DIR"].map { URL(fileURLWithPath: $0) }
      ?? FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/" + folder)
    return support.appendingPathComponent("host-core")
  }

  private static var repository: URL {
    #if DEBUG
    return URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../..", isDirectory: true).standardizedFileURL
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
      throw CoreHostFailure(code: "unavailable", message: "The host service executable is missing: \(name)", requestID: nil)
    }
    return built
  }

  /// Observe the resident service without starting one just to quit it.
  public func activityBeforeQuit() async throws -> Bool {
    guard FileManager.default.fileExists(atPath: stateDirectory.appendingPathComponent("host.ready").path) else { return false }
    let activity: Tsugumi_Host_HostActivityT = try await layoutCall(
      .hostactivityrequest, Tsugumi_Host_HostActivityRequestT(), timeout: .seconds(3), existingOnly: true)
    return activity.busy
  }

  public func stopResident(timeout: Duration = .seconds(47)) async {
    HostProcessLifetime.shared.end()
    let file = stateDirectory.appendingPathComponent("host.ready")
    guard let data = try? Data(contentsOf: file),
          let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let pid = value["pid"] as? Int32, pid > 1 else { return }
    HostProcessLifetime.shared.requestStop(pid)
    let deadline = ContinuousClock.now + timeout
    while kill(pid, 0) == 0 && ContinuousClock.now < deadline {
      try? await Task.sleep(for: .milliseconds(100))
    }
  }

  private func ready(existingOnly: Bool = false) async throws -> [String: Any] {
    let state = stateDirectory
    let file = state.appendingPathComponent("host.ready")
    func read() -> [String: Any]? {
      guard let bytes = try? Data(contentsOf: file),
            let object = try? JSONSerialization.jsonObject(with: bytes) as? [String: Any],
            let pid = object["pid"] as? Int32, kill(pid, 0) == 0,
            object["protocol"] as? Int == 1, object["unix_socket"] is String, object["host_socket"] is String, object["user_token"] is String else { return nil }
      return object
    }
    if let existing = read() { return existing }
    if existingOnly { throw RPCError.disconnected }
    let socketRoot = ProcessInfo.processInfo.environment["TSUGUMI_SOCKET_DIR"] ?? "/private/tmp"
    let directory = URL(fileURLWithPath: socketRoot, isDirectory: true).appendingPathComponent("tsugumi-host-\(UUID().uuidString.prefix(8))")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    let daemon = Process()
    daemon.executableURL = try executable("tsugumi-hostd", environment: "TSUGUMI_HOSTD", component: "hostd")
    let demos = Bundle.main.url(forResource: "demos", withExtension: nil) ?? Self.repository.appendingPathComponent("demos")
    daemon.arguments = ["--state-dir", state.path, "--unix", directory.appendingPathComponent("agent.sock").path,
                        "--listen", "127.0.0.1:0", "--demos-dir", demos.path]
    daemon.arguments! += runtimeArguments ?? HostRuntimeConfiguration.arguments(support: state.deletingLastPathComponent())
    // The service creates and numbers its state folder. Until then, its usual
    // stderr log lives beside state (on the same filesystem) until startup succeeds.
    let stateLog = state.appendingPathComponent("host.log")
    let parent = state.deletingLastPathComponent()
    try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    let log = FileManager.default.fileExists(atPath: state.path) ? stateLog : parent.appendingPathComponent(".host-startup-\(UUID().uuidString).log")
    if !FileManager.default.fileExists(atPath: log.path) { FileManager.default.createFile(atPath: log.path, contents: nil, attributes: [.posixPermissions: 0o600]) }
    let handle = try FileHandle(forWritingTo: log)
    try handle.seekToEnd()
    daemon.standardOutput = handle
    daemon.standardError = handle
    daemon.standardInput = FileHandle.nullDevice
    try daemon.run()
    HostProcessLifetime.shared.launched(daemon.processIdentifier)
    defer { HostProcessLifetime.shared.becameReady(daemon.processIdentifier) }
    try handle.close()
    let value = try await HostReadiness.wait(read: read, exitStatus: {
      daemon.isRunning ? nil : daemon.terminationStatus
    }, log: log, state: state)
    if log != stateLog, value["pid"] as? Int32 == daemon.processIdentifier {
      if rename(log.path, stateLog.path) != 0 {
        throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
      }
    }
    return value
  }

  private func connect(existingOnly: Bool = false) async throws {
    guard !HostProcessLifetime.shared.isEnding else { throw CancellationError() }
    if let opening { return try await opening.value }
    if !transports.isEmpty { return }
    let task = Task { try await self.start(existingOnly: existingOnly) }
    opening = task
    do { try await task.value; opening = nil }
    catch { opening = nil; close(); throw error }
  }

  private func start(existingOnly: Bool = false) async throws {
    let ready = try await ready(existingOnly: existingOnly)
    // Quit may have started while the launched process was publishing readiness.
    if HostProcessLifetime.shared.isEnding {
      if let pid = ready["pid"] as? Int32, pid > 1 { HostProcessLifetime.shared.requestStop(pid) }
      throw CancellationError()
    }
    guard let agentSocket = ready["unix_socket"] as? String, let hostSocket = ready["host_socket"] as? String,
          let token = ready["user_token"] as? String else {
      throw CoreHostFailure(code: "protocol", message: "Invalid service endpoints", requestID: nil)
    }
    let identity = UUID()
    connectionID = identity
    for host in [false, true] {
      let transport = NativeByteTransport(endpoint: "unix:" + (host ? hostSocket : agentSocket), contract: host ? .host : .agent)
      transports[host] = transport
      transport.observe(receive: { [weak self] data in
        if host { Task { await self?.hostEvent(data, identity: identity) } }
      }, closed: { [weak self] in
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

  /// Releases both connections before switching to another service instance.
  func disconnect() async {
    _ = try? await opening?.value
    close()
  }

  private func closed(_ identity: UUID) {
    guard identity == connectionID else { return }
    close()
    for handler in hostObservers.values { handler(["type": "Disconnected"]) }
  }

  private func exchange(_ method: String, _ fields: [String: Any], host: Bool) async throws -> [String: Any] {
    guard let transport = transports[host], nextID < UInt64.max else {
      throw CoreHostFailure(code: "disconnected", message: "The service connection is closed", requestID: nil)
    }
    nextID += 1
    let frame = try ServiceWire.encode(method, fields: fields, id: nextID, host: host)
    let identity = connectionID
    let data: Data
    do {
      data = try await withCheckedThrowingContinuation { continuation in
        let stoppingRuntime = method == "HostRuntimeRequest" && (fields["action"] as? Tsugumi_Host_RuntimeAction) == .shutdown
        transport.call(frame, timeout: .seconds(stoppingRuntime ? 45 : ["EnvironmentRequest", "AttachFoldersRequest", "HostRuntimeRequest"].contains(method) ? 3600 : 120), continuation: continuation)
      }
    } catch RPCError.disconnected {
      // The pending reply can fail before the close observer reaches this
      // actor. A snapshot read after restart must open a fresh connection.
      closed(identity)
      throw RPCError.disconnected
    }
    return try ServiceWire.decode(data, host: host)
  }

  public func call(_ method: String, _ fields: [String: Any] = [:]) async throws -> [String: Any] {
    try await connect()
    return try await exchange(method, fields, host: ServiceWire.hostMethod(method, fields))
  }

  /// Stateless presentation operations use the host contract, independently of a project socket.
  public func layoutCall<T: NativeObject>(_ type: Tsugumi_Host_RequestPayload, _ value: NativeObject,
                                          timeout: Duration = .seconds(120), existingOnly: Bool = false) async throws -> T {
    try await connect(existingOnly: existingOnly)
    guard let transport = transports[true], nextID < UInt64.max else { throw RPCError.disconnected }
    nextID += 1
    let request = Tsugumi_Host_RequestT()
    request.value = Tsugumi_Host_RequestPayloadUnion(value, type: type)
    let message = Tsugumi_Host_MessageT()
    message.requestId = nextID
    message.payload = Tsugumi_Host_PayloadUnion(request, type: .request)
    var builder = FlatBufferBuilder(initialSize: 1024)
    var object: Tsugumi_Host_MessageT? = message
    let offset = Tsugumi_Host_Message.pack(&builder, obj: &object)
    Tsugumi_Host_Message.finish(&builder, end: offset)
    let data: Data = try await withCheckedThrowingContinuation { continuation in
      transport.call(builder.data, timeout: timeout, continuation: continuation)
    }
    var buffer = ByteBuffer(data: data)
    let root: Tsugumi_Host_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSH1")
    let result = (Tsugumi_Host_MessageT(root).payload?.value as? Tsugumi_Host_ReplyT)?.value?.value
    if let failure = result as? Tsugumi_Service_FailureT {
      throw CoreHostFailure(code: failure.code, message: failure.message, requestID: failure.requestId)
    }
    guard let result = result as? T else { throw CoreHostFailure(code: "protocol", message: "Invalid layout reply", requestID: nil) }
    return result
  }

  public func completeLayout(project: String, layout: String, definitions: [String: String]) async throws -> String {
    let request = Tsugumi_Host_LayoutCompleteRequestT()
    request.projectJson = project; request.layoutJson = layout
    request.definitionsJson = String(decoding: try JSONEncoder().encode(definitions), as: UTF8.self)
    let result: Tsugumi_Host_LayoutValidatedT = try await layoutCall(.layoutcompleterequest, request)
    return result.json
  }

  public func operateProject(_ request: Tsugumi_Host_ProjectOperateRequestT) async throws -> Tsugumi_RPC_SaveResultT {
    let result: Tsugumi_Host_ProjectOperationResultT = try await layoutCall(.projectoperaterequest, request)
    if let failure = result.reply.value?.value as? Tsugumi_Data_FailureT { throw RPCFailure(failure) }
    guard let saved = result.reply.value?.value as? Tsugumi_RPC_SaveResultT else {
      throw RPCError.protocolViolation("Missing operation result")
    }
    return saved
  }

  public func validateLayout(_ json: String, removedCards: [String] = []) async throws -> String {
    let request = Tsugumi_Host_LayoutValidateRequestT()
    request.json = json; request.removedCards = removedCards.map(Optional.some)
    let result: Tsugumi_Host_LayoutValidatedT = try await layoutCall(.layoutvalidaterequest, request)
    return result.json
  }

  public func configure(_ config: ProviderConfig, conversationID: String) async throws {
    _ = try await call("ConfigureRequest", ["provider": try await provider(config), "conversation_id": conversationID])
  }

  public func provider(_ config: ProviderConfig, asking: Bool = true) async throws -> Tsugumi_Service_ProviderSelectionT {
    let value = Tsugumi_Service_ProviderSelectionT()
    value.model = config.model
    value.purpose = config.purpose
    value.useChoices = config.usesChoices
    value.profile = "desktop"
    value.providerId = config.connection
    value.accountProfile = "desktop"
    if config.auth == .apiKey {
      let key = asking ? Keychain.load(account: config.name) : Keychain.loadWithoutAsking(account: config.name)
      try await provisionKey(key, id: config.connection)
      value.keyProfile = keyProfile
    }
    return value
  }

  public func format(path: String, text: String) async throws -> Tsugumi_Host_FileFormatResultT {
    let reply = try await call("FileFormatRequest", ["path": path, "text": text])
    guard let result = reply["result"] as? Tsugumi_Host_FileFormatResultT else { throw CoreHostFailure(code: "protocol", message: "Missing format result", requestID: nil) }
    return result
  }

  public func versionGraph(project: String, language: String) async throws -> Tsugumi_RPC_VersionGraphT {
    let reply = try await call("HostVersionGraphRequest", ["project": project, "language": language])
    guard let result = reply["result"] as? Tsugumi_RPC_VersionGraphT else { throw CoreHostFailure(code: "protocol", message: "Missing version graph", requestID: nil) }
    return result
  }

  public func versionDiff(project: String, commit: String, from: String?) async throws -> Tsugumi_Host_VersionDiffT {
    var fields: [String: Any] = ["project": project, "commit": commit]
    if let from { fields["from_commit"] = from }
    let reply = try await call("HostVersionDiffRequest", fields)
    guard let result = reply["result"] as? Tsugumi_Host_VersionDiffT else { throw CoreHostFailure(code: "protocol", message: "Missing version diff", requestID: nil) }
    return result
  }

  public func permissionKinds(language: String) async throws -> [Tsugumi_Service_PermissionKindT] {
    let result = try await call("PermissionKindsRequest", ["language": language])
    return result["kinds"] as? [Tsugumi_Service_PermissionKindT] ?? []
  }

  public func modelSelection(_ purpose: String) async throws -> Tsugumi_Service_ProviderSelectionT {
    let reply = try await call("ModelSelectionRequest", ["profile": "desktop", "purpose": purpose])
    guard let selected = reply["provider"] as? Tsugumi_Service_ProviderSelectionT else {
      throw CoreHostFailure(code: "protocol", message: "Invalid host model selection", requestID: nil)
    }
    return selected
  }

  public func usable(_ config: ProviderConfig) async throws -> Bool {
    let selected = try await provider(config, asking: false)
    let reply = try await call("ProviderStatusRequest", ["profile": "desktop", "id": config.connection,
      "key_profile": selected.keyProfile ?? ""])
    let usable = reply["configured"] as? Bool ?? false
    ModelCatalogSnapshot.shared.showAvailability(config.connection, usable)
    return usable
  }

  public func versionComments(project: String, commit: String) async throws -> [Tsugumi_Host_VersionCommentT] {
    let result = try await call("VersionCommentsRequest", ["project": project, "commit": commit])
    return result["comments"] as? [Tsugumi_Host_VersionCommentT] ?? []
  }

  /// Platform credential transport only; hostd chooses the note mode and model.
  public func versionNote(project: String, commit: String, language: String) async throws -> String {
    for (id, account) in [("anthropic", "Anthropic"), ("openai", "OpenAI")] {
      try await provisionKey(Keychain.loadWithoutAsking(account: account), id: id)
    }
    let reply = try await call("VersionNoteRequest", ["project_id": project, "commit": commit,
      "profile": "desktop", "language": language,
      "anthropic_key_profile": keyProfile, "openai_key_profile": keyProfile])
    return reply["text"] as? String ?? ""
  }

  public func provisionKey(_ key: String?, id: String) async throws {
    var fields: [String: Any] = ["profile": keyProfile, "id": id, "ephemeral": true, "source": "keychain", "remove": key == nil]
    if let key { let input = Tsugumi_Host_ProviderInputT(); input.apiKey = key; fields["provider"] = input }
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

/// A hostd that loses the state-directory lock exits with status 4. Another
/// client (Home, environment setup, or another process) is publishing readiness.
/// Its exit does not mean that the shared service failed to start.
enum HostReadiness {
  static func wait(read: () -> [String: Any]?, exitStatus: () -> Int32?, log: URL, state: URL? = nil,
                   deadline: Date = Date().addingTimeInterval(15),
                   pause: () async throws -> Void = { try await Task.sleep(for: .milliseconds(50)) }) async throws -> [String: Any] {
    while Date() < deadline {
      if let value = read() { return value }
      if let status = exitStatus(), status != 4 {
        if status == 5 {
          let folder = (state ?? log.deletingLastPathComponent()).standardizedFileURL.path
          let message = String(format: String(localized: "ホストの状態フォルダ「%@」は別のバージョンの Tsugumi で作成されました。このフォルダを移動または改名して退避し、Tsugumi を起動し直してください。", bundle: .main), folder)
          throw CoreHostFailure(code: "state_format", message: message, requestID: nil)
        }
        throw CoreHostFailure(code: "unavailable", message: "The host service exited during startup; see \(log.path)", requestID: nil)
      }
      try await pause()
    }
    throw CoreHostFailure(code: "timeout", message: "The host service did not become ready; see \(log.path)", requestID: nil)
  }
}
