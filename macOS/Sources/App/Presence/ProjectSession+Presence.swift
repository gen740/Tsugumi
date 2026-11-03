import Foundation
import TsugumiRPC

/// The Agent's saves as scenes of the tsugumi (AgentPresence).
extension ProjectSession {
  struct PresenceSnapshot {
    let existing: Set<String>
    let templates: [String: String]
    let connections: [PresenceEvents.Connection]
  }

  func presenceSnapshot() -> PresenceSnapshot {
    PresenceSnapshot(existing: Set(confirmedDefinitions.keys).union(confirmedLinks.keys),
                     templates: confirmedDefinitions.compactMapValues { $0["template"]?.string },
                     connections: topConnections(confirmedProject))
  }

  func notePresence(_ d: Tsugumi_RPC_DocumentDeltaT, before: PresenceSnapshot) {
    let cards = Set(confirmedProject["canvas"]?["cards"]?.array?.compactMap { $0["id"]?.string } ?? [])
    let sources = d.sourcesUpsert.compactMap { $0.flatMap { PresenceEvents.cardOfFile($0.path) } }
    let across = flow == .leftToRight
    let w = PresenceEvents.written(
      upserted: d.definitionsUpsert.compactMap { $0?.id }, touchedSources: sources, existedBefore: before.existing,
      templateBefore: before.templates, templateAfter: confirmedDefinitions.compactMapValues { $0["template"]?.string },
      cards: cards, connectionsBefore: before.connections, connectionsAfter: topConnections(confirmedProject),
      order: { id in let p = self.position(id); return Double(across ? p.x : p.y) })
    presence.name = { [weak self] k in self?.nodeData(k).title ?? k }
    presence.saved(written: w.cards.map { (key([$0.0]), $0.reveal) },
                   wires: w.wires.map { AgentPresence.Wire(from: key([$0.from]), fromPort: $0.fromPort, to: key([$0.to]), toPort: $0.toPort) })
  }

  private func topConnections(_ project: JSONValue) -> [PresenceEvents.Connection] {
    (project["canvas"]?["connections"]?.array ?? []).compactMap(PresenceEvents.Connection.init)
  }
}
