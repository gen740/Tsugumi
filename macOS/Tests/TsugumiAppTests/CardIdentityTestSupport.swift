import Foundation
@testable import TsugumiApp

/// Deterministic RPC boundary for presentation fixtures; core identity tests
/// exercise the real allocator and its salt independently.
@MainActor func withTestCardIds(_ session: ProjectSession) -> ProjectSession {
  session.prepareSubgraph = { project, cards, id in
    let env = ProcessInfo.processInfo.environment
    let build = env["TSUGUMI_BUILD_DIR"].map { URL(fileURLWithPath: $0) } ?? SyncTests.root.appendingPathComponent("build")
    let binary = env["TSUGUMI_DOCUMENT_EDIT_DRIVER"].map { URL(fileURLWithPath: $0) }
      ?? env["TSUGUMI_MOCK_DAEMON"].map { URL(fileURLWithPath: $0).deletingLastPathComponent().appendingPathComponent("tsugumi_document_edit_driver") }
      ?? build.appendingPathComponent("tests/core/tsugumi_document_edit_driver")
    let process = Process(), input = Pipe(), output = Pipe(), error = Pipe()
    process.executableURL = binary
    process.standardInput = input; process.standardOutput = output; process.standardError = error
    try process.run()
    try input.fileHandleForWriting.write(contentsOf: JSONSerialization.data(withJSONObject: ["projectJson": project, "cards": cards, "id": id]))
    try input.fileHandleForWriting.close()
    let bytes = output.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    guard process.terminationStatus == 0,
          let reply = try JSONSerialization.jsonObject(with: bytes) as? [String: String],
          let outer = reply["projectJson"], let inner = reply["externalJson"], let link = reply["linkJson"] else {
      throw NSError(domain: "HostDocumentEdit", code: 1, userInfo: [NSLocalizedDescriptionKey: String(decoding: error.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)])
    }
    return (outer, inner, link)
  }
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
