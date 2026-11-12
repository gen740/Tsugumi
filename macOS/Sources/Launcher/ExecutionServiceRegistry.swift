import Darwin
import Foundation

/// An SSH alias refers to the host user's SSH configuration.
public struct ExecutionServiceConnection: Codable, Identifiable, Equatable, Sendable {
  public var id = ""
  public var name = ""
  public var sshHost = ""
  public var gateway: String?
  public var nativeSystem: String?
  public var systems: [String]?

  public init() {}
}

public enum ExecutionServiceRegistry {
  public struct Invalid: LocalizedError {
    let message: String
    public var errorDescription: String? { message }
  }

  public static func validate(_ entries: [ExecutionServiceConnection]) throws {
    guard Set(entries.map(\.id)).count == entries.count else {
      throw Invalid(message: "Each execution service must have a distinct ID.")
    }
    for entry in entries {
      guard entry.id != "local", matches(entry.id, #"[a-z][a-z0-9-]{0,31}"#) else {
        throw Invalid(message: "Use a service ID of up to 32 lowercase ASCII letters, numbers and hyphens, starting with a letter. The ID local is reserved.")
      }
      guard !entry.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
            entry.name.count <= 80, entry.name.unicodeScalars.allSatisfy({ !CharacterSet.controlCharacters.contains($0) }) else {
        throw Invalid(message: "Enter a service name of up to 80 characters.")
      }
      guard matches(entry.sshHost, #"[A-Za-z0-9][A-Za-z0-9._-]{0,252}"#) else {
        throw Invalid(message: "Enter a host alias from your SSH configuration.")
      }
      if let gateway = entry.gateway {
        guard matches(gateway, #"/[A-Za-z0-9_./-]{1,1023}"#), !gateway.hasSuffix("/") else {
          throw Invalid(message: "Enter an absolute path to the gateway executable, without spaces or arguments.")
        }
      }
      if entry.nativeSystem != nil || entry.systems != nil {
        let known = Set(["aarch64-linux", "x86_64-linux"])
        guard let native = entry.nativeSystem, let systems = entry.systems,
              systems.contains(native), Set(systems).isSubset(of: known) else {
          throw Invalid(message: "The execution service returned unsupported Linux systems.")
        }
      }
    }
  }

  public static func load(support: URL) throws -> [ExecutionServiceConnection] {
    let file = support.appendingPathComponent("execution-services.json")
    let data: Data
    do { data = try Data(contentsOf: file) }
    catch let error as CocoaError where error.code == .fileReadNoSuchFile { return [] }
    let entries = try JSONDecoder().decode([ExecutionServiceConnection].self, from: data)
    try validate(entries)
    return entries
  }

  public static func save(_ entries: [ExecutionServiceConnection], support: URL) throws {
    try validate(entries)
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

  private static func matches(_ value: String, _ pattern: String) -> Bool {
    value.range(of: "\\A(?:\(pattern))\\z", options: .regularExpression) != nil
  }

  private static func posixError() -> POSIXError { POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
}
