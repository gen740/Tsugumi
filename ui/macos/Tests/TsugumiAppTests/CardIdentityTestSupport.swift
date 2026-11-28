import Foundation
import FlatBuffers
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
    let normalized = name.lowercased().replacingOccurrences(of: #"[^a-z0-9_]+"#, with: "_", options: .regularExpression)
      .replacingOccurrences(of: #"^[^a-z]+"#, with: "", options: .regularExpression)
    let stem = normalized.isEmpty ? "card" : normalized
    let used = prefix.isEmpty ? Set(session.definitionDrafts.keys).union(session.cards(in: []).compactMap { $0["id"]?.string })
      : Set(session.external(prefix)?.definitions.keys.map { $0 } ?? [])
    var id = String(stem.prefix(64))
    var n = 2
    while used.contains(id) || issued.contains(prefix + id) {
      let suffix = "_\(n)"
      id = String(stem.prefix(64 - suffix.count)) + suffix
      n += 1
    }
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

func fixtureCardJSON(_ kind: CardKind, _ fields: [(String, JSONValue)] = []) -> String {
  var definition = implementedCard(kind, symbol: "none", systems: ["aarch64-linux"])
  for (key, value) in fields { definition[key] = value }
  return definition.serialized()
}

func fixtureProjectJSON(_ fields: String = "{}") -> String {
  var project: JSONValue = .object([
    ("formatVersion", .int(1)), ("name", .string("t")), ("nixPath", .array([])),
    ("trackedPaths", .array([])), ("agentExclude", .array([])), ("recordLogs", .bool(true)), ("recordIO", .bool(false)),
    ("logLimitBytes", .int(134217728)), ("outputLimitBytes", .int(68719476736)), ("outputLimitEntries", .int(1000)),
    ("canvas", .object([("cards", .array([])), ("connections", .array([]))])),
  ])
  for (key, value) in (try! JSONValue.parse(fields)).object ?? [] { project[key] = value }
  return project.serialized()
}

/// A session showing a document no host serves, for tests of what is drawn.
/// Editing needs a host: tests of actions use a Daemon or their own replies.
@MainActor func offlineSession(_ dir: URL, cards: [(id: String, card: String)], layout: String = "{}") throws -> ProjectSession {
  try Data(fixtureProjectJSON().utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  session.prepareLayout = { _, layout, _ in layout }
  let snapshot = Tsugumi_RPC_ProjectSnapshotT()
  let ids = cards.map { #"{"id":"\#($0.id)"}"# }.joined(separator: ",")
  snapshot.rev = 0; snapshot.projectJson = fixtureProjectJSON(#"{"canvas":{"cards":[\#(ids)],"connections":[]}}"#)
  snapshot.layout = fixtureLayout(layout)
  snapshot.definitions = cards.map { card in
    let definition = Tsugumi_RPC_DefinitionT(); definition.id = card.id; definition.cardJson = card.card
    definition.readme = Documents.printReadme(title: card.id, description: "", body: "")
    return definition
  }
  snapshot.run = Tsugumi_RPC_RunStatusT()
  session.receiveProjectSnapshot(snapshot, projectId: "p")
  return session
}

func fixtureLayout(_ text: String) -> [UInt8] {
  guard let view = try? JSONValue.parse(text) else { return [] }
  var layout = Tsugumi_Layout_LayoutT()
  layout.boundary = nil
  layout.cards = (view["cards"]?.object ?? []).map { id, placement in
    let card = Tsugumi_Layout_CardPlaceT()
    card.key = id
    card.x = placement["x"]?.double
    card.y = placement["y"]?.double
    card.collapsed = placement["collapsed"]?.bool
    card.inputOrder = placement["inputOrder"]?.array?.compactMap(\.string) ?? []
    card.outputOrder = placement["outputOrder"]?.array?.compactMap(\.string) ?? []
    card.junctions = (placement["junctions"]?.array ?? []).map { value in
      let junction = Tsugumi_Layout_JunctionT()
      junction.port = value["port"]?.string ?? ""
      junction.x = value["x"]?.double
      junction.y = value["y"]?.double
      return junction
    }
    return card
  }
  layout.notes = (view["notes"]?.array ?? []).map { value in
    let note = Tsugumi_Layout_NotePlaceT()
    note.id = value["id"]?.string ?? ""
    note.scope = value["scope"]?.string
    note.color = value["color"]?.string
    note.x = value["x"]?.double
    note.y = value["y"]?.double
    note.width = value["width"]?.double
    note.height = value["height"]?.double
    note.fontSize = value["fontSize"]?.double
    return note
  }
  layout.groups = (view["groups"]?.array ?? []).map { value in
    let group = Tsugumi_Layout_CardGroupT()
    group.id = value["id"]?.string ?? ""
    group.scope = value["scope"]?.string
    group.name = value["name"]?.string
    group.color = value["color"]?.string
    group.cards = value["cards"]?.array?.compactMap(\.string) ?? []
    return group
  }
  var builder = FlatBufferBuilder(initialSize: 256)
  let root = Tsugumi_Layout_Layout.pack(&builder, obj: &layout)
  Tsugumi_Layout_Layout.finish(&builder, end: root)
  return builder.sizedByteArray
}
