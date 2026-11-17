import Foundation

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

