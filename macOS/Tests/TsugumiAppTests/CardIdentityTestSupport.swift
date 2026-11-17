import Foundation
import TsugumiRPC
import TsugumiAgentClient
import TsugumiLauncher
@testable import TsugumiApp

/// Deterministic RPC boundary for presentation fixtures; core identity tests
/// exercise the real allocator independently.
@MainActor func withTestCardIds(_ session: ProjectSession) -> ProjectSession {
  session.operateProject = { [weak session] request in
    guard let session, let endpoint = session.endpointOverride else { throw RPCError.disconnected }
    _ = try await CoreHostClient.shared.call("RegisterProjectRequest", ["project_id": request.projectId,
      "endpoint": endpoint.url.absoluteString, "token": endpoint.token, "path": session.identity.url.path])
    return try await CoreHostClient.shared.operateProject(request)
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
  var references: [String: Set<String>] = [:]
  session.referenceIdProvider = { kind, scope, occupied in
    let key = kind + ":" + scope
    var used = (references[key] ?? []).union(occupied)
    let prefix = kind == "comment" ? "" : kind + "-"
    var number = used.count + 1
    while used.contains(prefix + String(number)) { number += 1 }
    let id = prefix + String(number)
    used.insert(id)
    references[key] = used
    return id
  }
  return session
}

/// A session showing a document no host serves, for tests of what is drawn.
/// Editing needs a host: tests of actions use a Daemon or their own replies.
@MainActor func offlineSession(_ dir: URL, cards: [(id: String, card: String)], layout: String = "{}") throws -> ProjectSession {
  try Data("{}".utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  session.prepareLayout = { _, layout, _ in layout }
  let snapshot = Tsugumi_RPC_ProjectSnapshotT()
  let ids = cards.map { #"{"id":"\#($0.id)"}"# }.joined(separator: ",")
  snapshot.rev = 0; snapshot.projectJson = #"{"name":"t","canvas":{"cards":[\#(ids)],"connections":[]}}"#
  snapshot.layoutJson = layout
  snapshot.definitions = cards.map { card in
    let definition = Tsugumi_RPC_DefinitionT(); definition.id = card.id; definition.cardJson = card.card
    return definition
  }
  snapshot.run = Tsugumi_RPC_RunStatusT()
  session.receiveProjectSnapshot(snapshot, projectId: "p")
  return session
}
