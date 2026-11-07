import Foundation

/// A project's nixSources (tsugumi.json): flakes its Cards read as
/// inputs.sources.<name>. The same in web/src/model/nixSources.ts
/// (tests/fixtures/nix-source-refs.json).
enum NixSources {
  private static func address(_ ref: String) -> String { String(ref.prefix { $0 != "?" }) }

  /// The commit of a locked reference, shortened, when its address says it.
  static func shortCommit(_ ref: String) -> String? {
    guard let m = address(ref).firstMatch(of: #/\/([0-9a-f]{40})$/#) else { return nil }
    return String(m.1.prefix(7))
  }

  /// What to ask for to find a locked flake's newest commit: its address without the commit and the hash.
  static func unlockedRef(_ ref: String) -> String {
    address(ref).replacing(#/\/[0-9a-f]{40}$/#, with: "")
  }

  /// The name a flake gets by default: the last part of its address, as a Nix attribute.
  static func sourceName(_ ref: String) -> String {
    let trimmed = unlockedRef(ref).replacing(#/\/+$/#, with: "").replacing(#/\.git$/#, with: "")
    let last = trimmed.split(whereSeparator: { $0 == "/" || $0 == ":" }).last.map(String.init) ?? ""
    let name = last.replacing(#/[^A-Za-z0-9_-]/#, with: "-")
    return name.wholeMatch(of: #/[A-Za-z_].*/#) != nil ? name : "_\(name)"
  }
}
