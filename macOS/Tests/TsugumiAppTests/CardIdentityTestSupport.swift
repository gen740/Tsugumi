import Foundation
@testable import TsugumiApp

/// Deterministic RPC boundary for presentation fixtures; core identity tests
/// exercise the real allocator and its salt independently.
@MainActor func withTestCardIds(_ session: ProjectSession) -> ProjectSession {
  var issued: Set<String> = []
  session.cardIdProvider = { [weak session] name, prefix in
    guard let session else { throw CancellationError() }
    let stem = String(name.lowercased().map { $0.isLetter || $0.isNumber || "-_".contains($0) ? $0 : "-" })
    let used = prefix.isEmpty ? Set(session.definitionDrafts.keys).union(session.cards(in: []).compactMap { $0["id"]?.string })
      : Set(session.external(prefix)?.definitions.keys.map { $0 } ?? [])
    var id = stem.isEmpty ? "card" : stem
    var n = 2
    while used.contains(id) || issued.contains(prefix + id) { id = "\(stem)-\(n)"; n += 1 }
    issued.insert(prefix + id)
    return id
  }
  return session
}
