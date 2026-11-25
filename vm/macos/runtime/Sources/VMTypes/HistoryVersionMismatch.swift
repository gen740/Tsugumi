import Foundation

/// The daemon cannot open a state database written by a different version.
public struct HistoryVersionMismatch: Sendable, Equatable {
  public let found: String
  public let expected: String
  public let path: String?

  public init?(diagnostic: [String: Any]) {
    guard let kind = diagnostic["kind"] as? String,
          kind == "incompatible_history" || kind == "incompatible_state",
          let found = diagnostic["found"] as? String, !found.isEmpty,
          let expected = diagnostic["expected"] as? String, !expected.isEmpty else { return nil }
    self.found = found
    self.expected = expected
    self.path = (diagnostic["path"] as? String).flatMap { $0.isEmpty ? nil : $0 }
  }
}

