import Foundation
import TsugumiRPC

/// How artifacts read for people, as in ui/web/src/wire/flatjson.ts: the uuid,
/// with the core's label (`<invocation sequence>.<k>`) next to it when there is one.
enum ArtifactText {
  static func name(id: String, label: String?) -> String {
    guard let label, !label.isEmpty else { return id }
    return "\(id) (\(label))"
  }

  static func name(_ a: Tsugumi_RPC_ArtifactInfoT) -> String { name(id: a.artifactId, label: a.label) }

  /// What an id in a recorded value resolves to now.
  static func state(_ s: Tsugumi_Data_ArtifactState) -> String {
    switch s {
    case .available: return String(localized: "利用できます")
    default: return String(localized: "利用できません（実行後に残していないか、不明な ID）")
    }
  }

  /// One line of a value's artifact: its name and state, and its size while it is there.
  static func line(_ a: Tsugumi_Data_ValueArtifactT) -> String {
    var text = String(localized: "成果物 ") + name(id: a.id, label: a.label) + " · " + state(a.state)
    if a.state == .available, let size = a.sizeBytes { text += " · \(size) bytes" }
    return text
  }

  /// Which call made a listed artifact, then whether it expires, then its size and files.
  static func caption(_ a: Tsugumi_RPC_ArtifactInfoT) -> String {
    var parts = ["#\(a.invocation ?? 0)"]
    if !a.keep { parts.append(String(localized: "残さない（実行の終了時に削除）")) }
    return (parts + ["\(a.sizeBytes ?? 0) bytes", String(localized: "\(a.members.count) 件")]).joined(separator: " · ")
  }
}
