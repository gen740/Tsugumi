import Foundation

/// The nixpkgs this Tsugumi builds with (its flake.lock): new projects are
/// pinned to it, so a later Tsugumi with another nixpkgs leaves them as they
/// are. The pin is part of the project's versions.
enum NixpkgsPin {
  static let current: String? = {
    var locks: [URL] = []
    if let src = Bundle.main.url(forResource: "tsugumi-source", withExtension: nil) { locks.append(src.appendingPathComponent("flake.lock")) }
    #if DEBUG
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<5 { repo.deleteLastPathComponent() }
    locks.append(repo.appendingPathComponent("flake.lock"))
    #endif
    for url in locks {
      guard let data = try? Data(contentsOf: url),
            let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let nodes = obj["nodes"] as? [String: Any], let pkgs = nodes["nixpkgs"] as? [String: Any],
            let locked = pkgs["locked"] as? [String: Any], let rev = locked["rev"] as? String,
            let hash = locked["narHash"] as? String else { continue }
      return "https://github.com/NixOS/nixpkgs/archive/\(rev).tar.gz#\(hash)"
    }
    return nil
  }()
}
