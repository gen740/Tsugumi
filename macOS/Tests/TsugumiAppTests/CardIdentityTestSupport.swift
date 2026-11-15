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
  session.prepareTemplate = { template, id in
    let reply = try hostDocumentReply(["template": template, "id": id])
    return (reply["definitionJson"] as! String, (reply["files"] as? [[String: String]] ?? []).map { (path: $0["path"]!, text: $0["text"]!) })
  }
  session.prepareConnect = { state, from, to, forkId in
    var fields: [String: Any] = ["stateJson": state, "fromCard": from.0, "fromPort": from.1, "toCard": to.0, "toPort": to.1]
    fields["forkId"] = forkId
    let reply = try hostDocumentReply(fields)
    return (reply["stateJson"] as! String, reply["insertedId"] as? String ?? "", reply["needsFork"] as? Bool ?? false)
  }
  session.prepareCopy = { state, paths, from, to, name in
    var fields: [String: Any] = ["stateJson": state, "paths": paths, "from": from, "to": to]
    fields["name"] = name
    let reply = try hostDocumentReply(fields)
    return (reply["editsJson"] as! String, (reply["files"] as? [[String: String]] ?? []).map { (from: $0["from"]!, to: $0["to"]!) })
  }
  session.prepareMerge = { base, ours, theirs in
    let reply = try hostMergeReply(base: base, ours: ours, theirs: theirs)
    return reply
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

@MainActor func hostMergeReply(base: String, ours: String, theirs: String) throws -> (merged: String, conflicts: [String]) {
  let env = ProcessInfo.processInfo.environment
  let build = env["TSUGUMI_BUILD_DIR"].map { URL(fileURLWithPath: $0) } ?? SyncTests.root.appendingPathComponent("build")
  let binary = env["TSUGUMI_DOCUMENT_EDIT_DRIVER"].map { URL(fileURLWithPath: $0) }
    ?? env["TSUGUMI_MOCK_DAEMON"].map { URL(fileURLWithPath: $0).deletingLastPathComponent().appendingPathComponent("tsugumi_document_edit_driver") }
    ?? build.appendingPathComponent("tests/core/tsugumi_document_edit_driver")
  let process = Process(), input = Pipe(), output = Pipe(), error = Pipe()
  process.executableURL = binary
  process.standardInput = input; process.standardOutput = output; process.standardError = error
  try process.run()
  try input.fileHandleForWriting.write(contentsOf: JSONSerialization.data(withJSONObject: ["baseJson": base, "oursJson": ours, "theirsJson": theirs]))
  try input.fileHandleForWriting.close()
  let bytes = output.fileHandleForReading.readDataToEndOfFile()
  process.waitUntilExit()
  guard process.terminationStatus == 0,
        let reply = try JSONSerialization.jsonObject(with: bytes) as? [String: Any],
        let merged = reply["mergedJson"] as? String, let conflicts = reply["conflicts"] as? [String] else {
    throw NSError(domain: "HostDocumentMerge", code: 1, userInfo: [NSLocalizedDescriptionKey: String(decoding: error.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)])
  }
  return (merged, conflicts)
}

@MainActor func hostDocumentReply(_ fields: [String: Any]) throws -> [String: Any] {
  let env = ProcessInfo.processInfo.environment
  let build = env["TSUGUMI_BUILD_DIR"].map { URL(fileURLWithPath: $0) } ?? SyncTests.root.appendingPathComponent("build")
  let binary = env["TSUGUMI_DOCUMENT_EDIT_DRIVER"].map { URL(fileURLWithPath: $0) }
    ?? env["TSUGUMI_MOCK_DAEMON"].map { URL(fileURLWithPath: $0).deletingLastPathComponent().appendingPathComponent("tsugumi_document_edit_driver") }
    ?? build.appendingPathComponent("tests/core/tsugumi_document_edit_driver")
  let process = Process(), input = Pipe(), output = Pipe(), error = Pipe()
  process.executableURL = binary
  process.standardInput = input; process.standardOutput = output; process.standardError = error
  try process.run()
  try input.fileHandleForWriting.write(contentsOf: JSONSerialization.data(withJSONObject: fields))
  try input.fileHandleForWriting.close()
  let bytes = output.fileHandleForReading.readDataToEndOfFile()
  process.waitUntilExit()
  guard process.terminationStatus == 0, let reply = try JSONSerialization.jsonObject(with: bytes) as? [String: Any] else {
    throw NSError(domain: "HostDocumentEdit", code: 1, userInfo: [NSLocalizedDescriptionKey: String(decoding: error.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)])
  }
  return reply
}
