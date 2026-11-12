import CryptoKit
import Darwin
import Foundation

/// Connection details for a ready environment. The token stays in memory and
/// in the 0600 control file only.
public struct Endpoint: Sendable, Equatable {
  public let url: URL
  public let token: String
  public let projectId: String
  public let serverInstanceId: String
}

/// The daemon cannot open a history database written by a different version.
public struct HistoryVersionMismatch: Sendable, Equatable {
  public let found: String
  public let expected: String
  public var archiveInterrupted: Bool { found == "pending" }

  public init?(diagnostic: [String: Any]) {
    guard diagnostic["kind"] as? String == "incompatible_history",
          let found = diagnostic["found"] as? String, !found.isEmpty,
          let expected = diagnostic["expected"] as? String, !expected.isEmpty else { return nil }
    self.found = found
    self.expected = expected
  }

  public var diagnostic: [String: String] {
    ["kind": "incompatible_history", "found": found, "expected": expected]
  }
}

public enum EnvironmentState: Sendable, Equatable {
  case stopped
  case preparing(String)  // first-time preparation (image load, volume)
  case starting(String)
  case ready(Endpoint)
  case reconnecting
  case failed(message: String, recovery: String, log: String)
  case incompatibleHistory(HistoryVersionMismatch)

  public var label: String {
    switch self {
    case .stopped: return "Stopped"
    case .preparing(let s): return "Preparing: \(s)"
    case .starting(let s): return "Starting: \(s)"
    case .ready: return "Ready"
    case .reconnecting: return "Reconnecting"
    case .failed(let m, _, _): return "Failed: \(m)"
    case .incompatibleHistory(let issue): return "Incompatible history: \(issue.found), expected \(issue.expected)"
    }
  }
}

/// Resource limits of one environment (adjustable; defaults per the spec).
public struct EnvironmentLimits: Sendable, Codable, Equatable {
  public var cpus = 2
  public var memoryGiB = 4
  public var storeGiB = 20
  public init() {}
}

/// Identifies a project directory independent of the path used to open it.
public struct ProjectIdentity: Sendable, Hashable {
  public let id: String  // 16 hex chars derived from device + inode
  public let url: URL    // canonical path

  public init(url: URL) throws {
    let canonical = url.resolvingSymlinksInPath().standardizedFileURL
    var st = stat()
    guard stat(canonical.path, &st) == 0 else { throw LauncherError.invalidProject(url.path) }
    guard FileManager.default.fileExists(atPath: canonical.appendingPathComponent("tsugumi.json").path) else {
      throw LauncherError.invalidProject(url.path)
    }
    let key = "\(st.st_dev):\(st.st_ino)"
    id = SHA256.hash(data: Data(key.utf8)).prefix(8).map { String(format: "%02x", $0) }.joined()
    self.url = canonical
  }
}

/// Host-side exclusive lock serializing start/stop of one environment.
final class HostLock {
  private var fd: Int32 = -1
  init(path: URL) throws {
    try FileManager.default.createDirectory(at: path.deletingLastPathComponent(), withIntermediateDirectories: true)
    fd = open(path.path, O_RDWR | O_CREAT | O_CLOEXEC, 0o600)
    guard fd >= 0 else { throw LauncherError.locked }
  }
  func lock(wait: Bool) throws {
    if flock(fd, LOCK_EX | (wait ? 0 : LOCK_NB)) != 0 { throw LauncherError.locked }
  }
  deinit {
    if fd >= 0 { close(fd) }
  }
}
