import CryptoKit
import Darwin
import Foundation
@_exported import TsugumiVMTypes

/// Connection details for a ready environment. The token stays in memory and
/// in the 0600 control file only.
public struct Endpoint: Sendable, Equatable {
  public let url: URL
  public let token: String
  public let projectId: String
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

/// Identifies a project directory independent of the path used to open it.
public struct ProjectIdentity: Sendable, Hashable {
  public let id: String  // 16 hex chars derived from device + inode
  public let url: URL    // canonical path
  // Presentation hint; hostd independently authorizes the filesystem identity.
  public let supportsExternalExecution: Bool

  public init(url: URL) throws {
    let canonical = url.resolvingSymlinksInPath().standardizedFileURL
    var st = stat()
    guard stat(canonical.path, &st) == 0 else { throw LauncherError.invalidProject(url.path) }
    guard FileManager.default.fileExists(atPath: canonical.appendingPathComponent("tsugumi.json").path) else {
      throw LauncherError.invalidProject(url.path)
    }
    let key = "\(st.st_dev):\(st.st_ino)"
    id = SHA256.hash(data: Data(key.utf8)).prefix(8).map { String(format: "%02x", $0) }.joined()
    supportsExternalExecution = st.st_birthtimespec.tv_sec > 0
    self.url = canonical
  }
}

