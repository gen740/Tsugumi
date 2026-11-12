import Darwin
import Foundation

public struct ExecutionServiceCapabilities: Codable, Equatable, Sendable {
  public let serverId: String
  public let nativeSystem: String
  public let systems: [String]
  public let isolation: String
  public let resourceLimits: Bool
  public let maxConcurrentJobs: Int
  public let workerContract: Int
}

/// Settings may display a verified service without reading its token.
public struct ExecutionServiceConnection: Codable, Identifiable, Equatable, Sendable {
  public let id: String
  public let name: String
  public let endpoint: String
  public let capabilities: ExecutionServiceCapabilities
}

public struct ExecutionServiceRegistration: Sendable {
  public var id = ""
  public var name = ""
  public var endpoint = ""
  public var token: String?

  public init() {}
}

struct StoredExecutionService: Codable, Equatable {
  let id: String
  let name: String
  let endpoint: String
  let token: String
  let capabilities: ExecutionServiceCapabilities

  var connection: ExecutionServiceConnection {
    ExecutionServiceConnection(id: id, name: name, endpoint: endpoint, capabilities: capabilities)
  }
}

public enum ExecutionServiceRegistry {
  static let workerContract = 1

  public struct Invalid: LocalizedError {
    let message: String
    public var errorDescription: String? { message }
  }

  static func endpointURL(_ value: String) throws -> URL {
    let invalid = Invalid(message: String(localized: "HTTPS の接続先、またはループバックアドレスの HTTP 接続先を入力してください。", bundle: .main))
    let value = value.trimmingCharacters(in: .whitespacesAndNewlines)
    guard value.count <= 2048, matches(value, #"https?://[^/@?#\\\s]+/?"#),
          var parts = URLComponents(string: value), let host = parts.host,
          matches(host, #"(?:[A-Za-z0-9.-]+|\[[A-Fa-f0-9:]+\])"#),
          parts.port.map({ (1...65535).contains($0) }) ?? true else { throw invalid }
    if parts.scheme == "http", !matches(value, #"http://(?:127\.0\.0\.1|\[::1\])(?::[0-9]+)?/?"#) { throw invalid }
    parts.host = host.lowercased()
    if parts.port == (parts.scheme == "https" ? 443 : 80) { parts.port = nil }
    parts.path = ""
    guard let url = parts.url else { throw invalid }
    return url
  }

  static func validate(_ value: ExecutionServiceRegistration) throws -> ExecutionServiceRegistration {
    guard validId(value.id) else {
      throw Invalid(message: "Use a service ID of up to 32 lowercase ASCII letters, numbers and hyphens, starting with a letter. The ID local is reserved.")
    }
    guard !value.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
          value.name.count <= 80, value.name.unicodeScalars.allSatisfy({ !CharacterSet.controlCharacters.contains($0) }) else {
      throw Invalid(message: "Enter a service name of up to 80 characters.")
    }
    if let token = value.token, !matches(token, #"[a-f0-9]{64}"#) {
      throw Invalid(message: String(localized: "認証トークンは小文字の16進数64文字です。", bundle: .main))
    }
    var checked = value
    checked.endpoint = try endpointURL(value.endpoint).absoluteString
    return checked
  }

  static func validate(_ caps: ExecutionServiceCapabilities) throws {
    let known = Set(["aarch64-linux", "x86_64-linux"])
    guard matches(caps.serverId, #"[a-f0-9]{64}"#), caps.systems.contains(caps.nativeSystem),
          Set(caps.systems).isSubset(of: known), caps.isolation == "pid_namespace",
          (1...Int(UInt32.max)).contains(caps.maxConcurrentJobs), caps.workerContract > 0 else {
      throw Invalid(message: String(localized: "実行デーモンが有効な実行環境情報を返しませんでした。", bundle: .main))
    }
  }

  public static func load(support: URL) throws -> [ExecutionServiceConnection] {
    try loadStored(support: support).map(\.connection)
  }

  static func loadStored(support: URL) throws -> [StoredExecutionService] {
    let file = support.appendingPathComponent("execution-services.json")
    let fd = open(file.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
    if fd < 0, errno == ENOENT { return [] }
    guard fd >= 0 else { throw posixError() }
    let handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
    defer { try? handle.close() }
    var st = stat()
    guard fstat(fd, &st) == 0, st.st_mode & S_IFMT == S_IFREG, st.st_uid == getuid(),
          st.st_mode & 0o077 == 0, st.st_size <= 1 << 20 else {
      throw Invalid(message: "The execution service registry must be a private host file.")
    }
    let entries = try JSONDecoder().decode([StoredExecutionService].self, from: handle.readToEnd() ?? Data())
    guard Set(entries.map(\.id)).count == entries.count else {
      throw Invalid(message: "Each execution service must have a distinct ID.")
    }
    for entry in entries {
      var input = ExecutionServiceRegistration()
      input.id = entry.id
      input.name = entry.name
      input.endpoint = entry.endpoint
      input.token = entry.token
      _ = try validate(input)
      try validate(entry.capabilities)
    }
    return entries
  }

  public static func register(_ input: ExecutionServiceRegistration, support: URL) async throws -> [ExecutionServiceConnection] {
    let entry = try validate(input)
    let previous = try loadStored(support: support).first { $0.id == entry.id }
    guard let token = entry.token ?? (previous?.endpoint == entry.endpoint ? previous?.token : nil) else {
      throw Invalid(message: String(localized: "この接続先の認証トークンを入力してください。", bundle: .main))
    }
    let caps = try await Task.detached { try ExecutionServiceBroker.probe(entry, token: token) }.value
    let entries = try loadStored(support: support)
    guard entries.first(where: { $0.id == entry.id }) == previous else {
      throw Invalid(message: String(localized: "登録内容が更新されました。もう一度設定を開いてください。", bundle: .main))
    }
    let verified = StoredExecutionService(id: entry.id, name: entry.name, endpoint: entry.endpoint, token: token, capabilities: caps)
    let next = entries.filter { $0.id != entry.id } + [verified]
    try saveStored(next, support: support)
    return next.map(\.connection)
  }

  public static func remove(id: String, support: URL) throws -> [ExecutionServiceConnection] {
    guard validId(id) else { throw Invalid(message: "Invalid execution service ID.") }
    let entries = try loadStored(support: support).filter { $0.id != id }
    try saveStored(entries, support: support)
    return entries.map(\.connection)
  }

  static func saveStored(_ entries: [StoredExecutionService], support: URL) throws {
    try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true,
                                            attributes: [.posixPermissions: 0o700])
    let destination = support.appendingPathComponent("execution-services.json")
    let temporary = support.appendingPathComponent(".execution-services-\(UUID().uuidString)")
    let fd = open(temporary.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600)
    guard fd >= 0 else { throw posixError() }
    defer { close(fd); unlink(temporary.path) }
    try writeAll(fd, JSONEncoder().encode(entries))
    guard rename(temporary.path, destination.path) == 0 else { throw posixError() }
  }

  private static func validId(_ id: String) -> Bool { id != "local" && matches(id, #"[a-z][a-z0-9-]{0,31}"#) }

  private static func matches(_ value: String, _ pattern: String) -> Bool {
    value.range(of: "\\A(?:\(pattern))\\z", options: .regularExpression) != nil
  }

  private static func posixError() -> POSIXError { POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
}
