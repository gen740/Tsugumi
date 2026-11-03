import CryptoKit
import Foundation

/// Folders of the Mac a project is given in the VM (Experimental): those its
/// Cards name in hostFolders (its own definitions and its externals') and any
/// added by the app (a chat's attached folders). tsugumid finds each at
/// /run/tsugumi/host/<tag> with the same tag.
public enum HostFolders {
  public struct Folder: Hashable, Sendable {
    public let path: String
    public let write: Bool
  }

  /// sha256 of the path, first 16 hex digits (as tsugumid computes it).
  public static func tag(_ path: String) -> String {
    SHA256.hash(data: Data(path.utf8)).prefix(8).map { String(format: "%02x", $0) }.joined()
  }

  /// Existing folders the project needs, writable when any Card writes there.
  public static func wanted(project: URL, extra: [String]) -> [Folder] {
    var write: [String: Bool] = [:]
    for card in cardFiles(project) {
      guard let data = try? Data(contentsOf: card),
            let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let folders = obj["hostFolders"] as? [[String: Any]] else { continue }
      for f in folders {
        guard let path = f["path"] as? String, path.hasPrefix("/") else { continue }
        write[path] = (write[path] ?? false) || (f["write"] as? Bool ?? false)
      }
    }
    for path in extra where path.hasPrefix("/") { write[path] = write[path] ?? false }
    var isDir: ObjCBool = false
    return write.keys.sorted().filter { FileManager.default.fileExists(atPath: $0, isDirectory: &isDir) && isDir.boolValue }
      .map { Folder(path: $0, write: write[$0]!) }
  }

  /// definitions/*/_card.json of the project and of its externals, nested.
  static func cardFiles(_ root: URL, depth: Int = 0) -> [URL] {
    let fm = FileManager.default
    var out: [URL] = []
    let defs = root.appendingPathComponent("definitions")
    for id in (try? fm.contentsOfDirectory(atPath: defs.path)) ?? [] {
      out.append(defs.appendingPathComponent(id).appendingPathComponent("_card.json"))
    }
    guard depth < 15 else { return out }
    let ext = root.appendingPathComponent("externals")
    for name in (try? fm.contentsOfDirectory(atPath: ext.path)) ?? [] {
      out += cardFiles(ext.appendingPathComponent(name), depth: depth + 1)
    }
    return out
  }
}
