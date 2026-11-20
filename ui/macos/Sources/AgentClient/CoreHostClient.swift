import FlatBuffers
import Darwin
import Foundation
import TsugumiRPC

public struct AgentProject: Sendable {
  public let id: String
  public init(id: String) { self.id = id }
}

public struct CoreHostFailure: LocalizedError, CustomStringConvertible, Sendable {
  public let code: String
  public let message: String
  public let requestID: String?
  public var description: String {
    switch code {
    case "stale_project": return String(localized: "このフォルダのプロジェクト ID が変更または削除されました。このプロジェクトを閉じて開き直し、現在の ID で登録してください。会話の記録は削除していません。")
    case "conversation_identity_mismatch": return String(localized: "このプロジェクトの会話記録は別のプロジェクト ID に属しています。バックアップから対応する .tsugumi フォルダを復元してください。新しい会話を始める場合は、プロジェクトを閉じ、agent.sqlite を .tsugumi の外に移動して保管してから開き直してください。会話の記録は削除していません。")
    default: return ChatGPTError.localizedMessage(message, code: code)
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
    // A launch completing during Quit shares the stopResident signal.
    if lock.withLock({ signalled.insert(pid).inserted }) { _ = kill(pid, SIGTERM) }
  }
}

/// Binary Agent and Host connections share the project's C ABI byte transport.
/// Framing, verification, deadlines and pending requests belong to client/.
public actor CoreHostClient {
  public static let shared = CoreHostClient()
  private let supportOverride: URL?

  public init(support: URL? = nil) { supportOverride = support }
  private var transports: [Bool: NativeByteTransport] = [:]
  private var connectionID = UUID()
  private var hostConnected = false
  private var opening: Task<Void, Error>?
  private var nextID: UInt64 = 0
  private var runtimeArguments: [String]?
  private var hostObservers: [UUID: @Sendable ([String: Any]) -> Void] = [:]
  public func configureRuntime(_ arguments: [String]) { runtimeArguments = arguments }
  public func observeHost(_ handler: @escaping @Sendable ([String: Any]) -> Void) -> UUID {
    let token = UUID(); hostObservers[token] = handler; return token
  }
  func signInChanges(profile: String) async -> AsyncStream<Void> {
    let (events, continuation) = AsyncStream<Void>.makeStream(bufferingPolicy: .bufferingNewest(1))
    let token = observeHost { event in
      switch event["type"] as? String {
      case "SignInChanged" where event["profile"] as? String == profile, "Disconnected":
        continuation.yield(())
      default: break
      }
    }
    continuation.onTermination = { [weak self] _ in
      Task { await self?.removeHostObserver(token) }
    }
    return events
  }

  /// A new identity restarts a panel even if SwiftUI coalesces a disconnect
  /// and a reconnect into one update.
  public func hostConnectionChanges() -> AsyncStream<UUID?> {
    let (stream, continuation) = AsyncStream<UUID?>.makeStream()
    continuation.yield(hostConnected ? connectionID : nil)
    let observer = observeHost { event in
      switch event["type"] as? String {
      case "Connected": continuation.yield(UUID())
      case "Disconnected": continuation.yield(nil)
      default: break
      }
    }
    continuation.onTermination = { [weak self] _ in
      Task { await self?.removeHostObserver(observer) }
    }
    return stream
  }

  private func removeHostObserver(_ observer: UUID) {
    hostObservers.removeValue(forKey: observer)
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
    let activity: Tsugumi_Host_HostActivityT = try await hostCall(
      .hostactivityrequest, Tsugumi_Host_HostActivityRequestT(), existingOnly: true)
    return activity.busy
  }

  public func stopResident(waitForExit: Bool = true) async {
    HostProcessLifetime.shared.end()
    let file = stateDirectory.appendingPathComponent("host.ready")
    guard let data = try? Data(contentsOf: file),
          let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let pid = value["pid"] as? Int32, pid > 1 else { return }
    if !waitForExit {
      HostProcessLifetime.shared.requestStop(pid)
      return
    }
    await withCheckedContinuation { continuation in
      let exit = DispatchSource.makeProcessSource(identifier: pid, eventMask: .exit, queue: .global())
      exit.setEventHandler { exit.cancel() }
      exit.setCancelHandler { continuation.resume() }
      exit.activate()
      if kill(pid, 0) == 0 { HostProcessLifetime.shared.requestStop(pid) }
      else { exit.cancel() }
    }
  }

  private func ready(existingOnly: Bool = false) async throws -> [String: Any] {
    let state = stateDirectory
    let file = state.appendingPathComponent("host.ready")
    func read() -> [String: Any]? {
      guard let bytes = try? Data(contentsOf: file),
            let object = try? JSONSerialization.jsonObject(with: bytes) as? [String: Any] else { return nil }
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
    daemon.arguments = ["--ensure", "--state-dir", state.path, "--unix", directory.appendingPathComponent("agent.sock").path,
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
    let output = Pipe()
    daemon.standardOutput = output
    daemon.standardError = handle
    daemon.standardInput = FileHandle.nullDevice
    try daemon.run()
    HostProcessLifetime.shared.launched(daemon.processIdentifier)
    defer { HostProcessLifetime.shared.becameReady(daemon.processIdentifier) }
    try handle.close()
    var value: [String: Any]?
    for try await line in output.fileHandleForReading.bytes.lines {
      value = try JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any]
      break
    }
    guard let value else {
      throw CoreHostFailure(code: "unavailable", message: "The host service exited during startup; see \(log.path)", requestID: nil)
    }
    if log != stateLog, value["pid"] as? Int32 == daemon.processIdentifier {
      if rename(log.path, stateLog.path) != 0 {
        throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
      }
    }
    return value
  }

  public func traceConnection() async throws -> HostTraceConnection {
    let ready = try await ready(existingOnly: true)
    guard let socket = ready["host_socket"] as? String, let token = ready["user_token"] as? String else {
      throw CoreHostFailure(code: "protocol", message: "Invalid service endpoints", requestID: nil)
    }
    return await HostTraceConnection(socket: socket, token: token)
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
      _ = try await exchange("HelloRequest", ["token": token], host: host)
    }
    hostConnected = true
    for handler in hostObservers.values { handler(["type": "Connected"]) }
  }

  private func close() {
    hostConnected = false
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
    try await ServiceWire.decode(exchangeFrame(method, fields, host: host), host: host)
  }

  private func exchangeFrame(_ method: String, _ fields: [String: Any], host: Bool) async throws -> Data {
    guard let transport = transports[host], nextID < UInt64.max else {
      throw CoreHostFailure(code: "disconnected", message: "The service connection is closed", requestID: nil)
    }
    nextID += 1
    let frame = try ServiceWire.encode(method, fields: fields, id: nextID, host: host)
    return try await send(frame, via: transport, timeout: .zero)
  }

  private func send(_ frame: Data, via transport: NativeByteTransport, timeout: Duration) async throws -> Data {
    let identity = connectionID
    do {
      return try await withCheckedThrowingContinuation { continuation in
        transport.call(frame, timeout: timeout, continuation: continuation)
      }
    } catch RPCError.disconnected {
      // The pending reply can fail before the close observer reaches this
      // actor. A snapshot read after restart must open a fresh connection.
      closed(identity)
      throw RPCError.disconnected
    }
  }

  public func call(_ method: String, _ fields: [String: Any] = [:]) async throws -> [String: Any] {
    try await connect()
    return try await exchange(method, fields, host: ServiceWire.hostMethod(method, fields))
  }

  func agentReply(_ method: String, _ fields: [String: Any]) async throws -> NativeObject {
    try await connect()
    return try await ServiceWire.decodeReply(exchangeFrame(method, fields, host: false), host: false)
  }

  public func agentCall<T: NativeObject>(_ method: String, _ fields: [String: Any]) async throws -> T {
    guard let result = try await agentReply(method, fields) as? T else {
      throw RPCError.unexpectedReply(method)
    }
    return result
  }

  public func createConversation(projectID: String) async throws -> String {
    let result: Tsugumi_Agent_ConversationResultT = try await agentCall("ConversationCreateRequest", ["project_id": projectID])
    return result.conversation.id
  }

  /// Stateless presentation operations use the host contract, independently of a project socket.
  public func hostCall<T: NativeObject>(_ type: Tsugumi_Host_RequestPayload, _ value: NativeObject,
                                          timeout: Duration = .zero, existingOnly: Bool = false) async throws -> T {
    try await connect(existingOnly: existingOnly)
    guard let transport = transports[true], nextID < UInt64.max else { throw RPCError.disconnected }
    nextID += 1
    let frame = ServiceWire.encodeHost(type, value, id: nextID)
    let data = try await send(frame, via: transport, timeout: timeout)
    let result = try ServiceWire.decodeReply(data, host: true)
    guard let result = result as? T else { throw CoreHostFailure(code: "protocol", message: "Invalid Host reply", requestID: nil) }
    return result
  }

  public func completeLayout(project: String, layout: String, definitions: [String: String]) async throws -> String {
    let request = Tsugumi_Host_LayoutCompleteRequestT()
    request.projectJson = project; request.layoutJson = layout
    request.definitionsJson = String(decoding: try JSONEncoder().encode(definitions), as: UTF8.self)
    let result: Tsugumi_Host_LayoutValidatedT = try await hostCall(.layoutcompleterequest, request)
    return result.json
  }

  public func operateProject(_ request: Tsugumi_Host_ProjectOperateRequestT) async throws -> Tsugumi_RPC_SaveResultT {
    let result: Tsugumi_Host_ProjectOperationResultT = try await hostCall(.projectoperaterequest, request)
    if let failure = result.reply.value?.value as? Tsugumi_Data_FailureT { throw RPCFailure(failure) }
    guard let saved = result.reply.value?.value as? Tsugumi_RPC_SaveResultT else {
      throw RPCError.protocolViolation("Missing operation result")
    }
    return saved
  }

  public func configure(_ config: ProviderConfig, conversationID: String) async throws {
    _ = try await call("ConfigureRequest", ["provider": try await provider(config), "conversation_id": conversationID])
  }

  public func provider(_ config: ProviderConfig) async throws -> Tsugumi_Service_ProviderSelectionT {
    let value = Tsugumi_Service_ProviderSelectionT()
    value.model = config.model
    value.purpose = config.purpose
    value.useChoices = config.usesChoices
    value.profile = "desktop"
    value.providerId = config.connection
    value.accountProfile = "desktop"
    return value
  }

  public func format(path: String, text: String) async throws -> Tsugumi_Host_FileFormatResultT {
    let request = Tsugumi_Host_FileFormatRequestT(); request.path = path; request.text = text
    return try await hostCall(.fileformatrequest, request)
  }

  public func versionGraph(project: String, language: String) async throws -> Tsugumi_RPC_VersionGraphT {
    let request = Tsugumi_Host_HostVersionGraphRequestT(); request.project = project; request.language = language
    return try await hostCall(.hostversiongraphrequest, request)
  }

  public func versionDiff(project: String, commit: String, from: String?) async throws -> Tsugumi_RPC_VersionDiffT {
    let request = Tsugumi_Host_HostVersionDiffRequestT(); request.project = project; request.commit = commit; request.fromCommit = from
    return try await hostCall(.hostversiondiffrequest, request)
  }

  public func permissionKinds(language: String) async throws -> [Tsugumi_Service_PermissionKindT] {
    let request = Tsugumi_Host_PermissionKindsRequestT(); request.language = language
    let result: Tsugumi_Host_PermissionKindsT = try await hostCall(.permissionkindsrequest, request)
    return result.kinds.compactMap { $0 }
  }

  public func modelSelection(_ purpose: String) async throws -> Tsugumi_Service_ProviderSelectionT {
    let request = Tsugumi_Host_ModelSelectionRequestT(); request.profile = "desktop"; request.purpose = purpose
    let result: Tsugumi_Host_ModelSelectionT = try await hostCall(.modelselectionrequest, request)
    return result.provider
  }

  public func usable(_ config: ProviderConfig) async throws -> Bool {
    try await availability(config).usable
  }

  public func availability(_ config: ProviderConfig) async throws -> (usable: Bool, code: String?) {
    let request = Tsugumi_Host_ProviderStatusRequestT(); request.profile = "desktop"; request.id = config.connection
    let reply: Tsugumi_Host_ProviderStatusT = try await hostCall(.providerstatusrequest, request)
    ModelCatalogSnapshot.shared.showAvailability(config.connection, reply.configured)
    return (reply.configured, reply.code)
  }

  public func versionComments(project: String, commit: String) async throws -> [Tsugumi_RPC_VersionCommentT] {
    let request = Tsugumi_Host_VersionCommentsRequestT(); request.project = project; request.commit = commit
    let result: Tsugumi_RPC_VersionCommentsT = try await hostCall(.versioncommentsrequest, request)
    return result.comments.compactMap { $0 }
  }

  /// hostd chooses the note mode, the model and its key.
  public func versionNote(project: String, commit: String, language: String) async throws -> String {
    let request = Tsugumi_Host_VersionNoteRequestT(); request.projectId = project; request.commit = commit
    request.profile = "desktop"; request.language = language
    let result: Tsugumi_Service_TextResultT = try await hostCall(.versionnoterequest, request)
    return result.text ?? ""
  }

  /// Stores (or with nil removes) a provider's API key in hostd.
  public func saveKey(_ key: String?, id: String) async throws {
    let request = Tsugumi_Host_ProviderSetRequestT(); request.profile = "desktop"; request.id = id
    request.remove = key == nil
    if let key { let input = Tsugumi_Host_ProviderInputT(); input.apiKey = key; request.provider = input }
    let result: Tsugumi_Host_ProviderStatusT = try await hostCall(.providersetrequest, request)
    ModelCatalogSnapshot.shared.showAvailability(id, result.configured)
  }

  public func storedModels(_ id: String) async throws -> [(id: String, name: String)] {
    try await modelList(id: id)
  }

  private func modelList(id: String, refresh: Bool = false) async throws -> [(id: String, name: String)] {
    let request = Tsugumi_Host_ModelsListRequestT(); request.providerId = id; request.refresh = refresh; request.profile = "desktop"
    let result: Tsugumi_Host_ModelListT = try await hostCall(.modelslistrequest, request)
    let list: [(id: String, name: String)] = result.models.compactMap { item in
      guard let item, let id = item.id else { return nil }
      return (id, item.name ?? id)
    }
    ModelCatalogSnapshot.shared.show(id, list)
    return list
  }

  public func models(_ config: ProviderConfig) async throws -> [(id: String, name: String)] {
    try await modelList(id: config.connection, refresh: true)
  }
}

/// A view owns this connection so cancelling the view also ends its subscription.
@MainActor
public final class HostTraceConnection {
  private nonisolated let transport: NativeByteTransport
  private let token: String
  private var nextID: UInt64 = 0

  fileprivate init(socket: String, token: String) {
    transport = NativeByteTransport(endpoint: "unix:" + socket, contract: .host)
    self.token = token
  }

  public func connect(changed: @escaping @Sendable (String?) -> Void,
                      closed: @escaping @Sendable () -> Void) async throws {
    transport.observe(receive: { data in
      guard let event = try? ServiceWire.hostEventValue(data),
            let notice = event as? Tsugumi_Trace_TraceChangedT else { return }
      changed(notice.catalog ? nil : notice.source ?? "")
    }, closed: closed)
    try await transport.start()
    let hello = Tsugumi_Service_HelloRequestT()
    hello.token = token
    hello.client = .trace
    _ = try await call(.hellorequest, hello)
  }

  public nonisolated func close() { transport.close() }

  public func query(mode: Tsugumi_Trace_Mode? = nil, source: String? = nil,
                    generation: UInt64 = 0, after: UInt64 = 0) async throws -> Tsugumi_Trace_HostTraceT {
    let request = Tsugumi_Trace_HostTraceRequestT()
    request.mode = mode?.rawValue
    request.source = source
    request.generation = generation
    request.after = after
    guard let result = try await call(.hosttracerequest, request) as? Tsugumi_Trace_HostTraceT else {
      throw RPCError.unexpectedReply("HostTrace")
    }
    return result
  }

  private func call(_ type: Tsugumi_Host_RequestPayload, _ value: NativeObject) async throws -> NativeObject {
    nextID += 1
    let frame = ServiceWire.encodeHost(type, value, id: nextID)
    let data = try await withCheckedThrowingContinuation { continuation in
      transport.call(frame, timeout: .zero, continuation: continuation)
    }
    return try ServiceWire.decodeReply(data, host: true)
  }
}
