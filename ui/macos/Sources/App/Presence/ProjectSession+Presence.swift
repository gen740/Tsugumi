import Foundation
import TsugumiRPC

/// The Agent's saves as scenes of the tsugumi (AgentPresence).
extension ProjectSession {
  struct PresenceSnapshot {
    let existing: Set<String>
    let types: [String: String]
    let connections: [PresenceEvents.Connection]
  }

  func presenceSnapshot() -> PresenceSnapshot {
    PresenceSnapshot(existing: Set(confirmedDefinitions.keys),
                     types: confirmedDefinitions.compactMapValues { $0["type"]?.string },
                     connections: topConnections(confirmedProject))
  }

  func notePresence(_ d: Tsugumi_RPC_DocumentDeltaT, before: PresenceSnapshot) {
    let cards = Set(confirmedProject["canvas"]?["cards"]?.array?.compactMap { $0["id"]?.string } ?? [])
    let sources = d.sourcesUpsert.compactMap { $0.flatMap { PresenceEvents.cardOfFile($0.path) } }
    let w = PresenceEvents.written(
      upserted: d.definitionsUpsert.compactMap { $0?.id }, touchedSources: sources, existedBefore: before.existing,
      typeBefore: before.types, typeAfter: confirmedDefinitions.compactMapValues { $0["type"]?.string },
      cards: cards, connectionsBefore: before.connections, connectionsAfter: topConnections(confirmedProject),
      order: { id in Double(self.position(id).y) })
    presence.name = { [weak self] k in self?.nodeData(k).title ?? k }
    // Each Card is written by its helper's bird (the main Agent's otherwise),
    // and a wire is threaded by the bird that wrote the Card it comes from.
    var order: [ObjectIdentifier] = []
    var parts: [ObjectIdentifier: (bird: AgentPresence, cards: [(CardKey, reveal: Bool)], wires: [AgentPresence.Wire])] = [:]
    func part(_ bird: AgentPresence) -> ObjectIdentifier {
      let id = ObjectIdentifier(bird)
      if parts[id] == nil { parts[id] = (bird, [], []); order.append(id) }
      return id
    }
    for c in w.cards { parts[part(presence(forCard: key([c.0])))]!.cards.append((key([c.0]), c.reveal)) }
    for x in w.wires {
      let from = presence(forCard: key([x.from]))
      let bird = from === presence ? presence(forCard: key([x.to])) : from
      parts[part(bird)]!.wires.append(AgentPresence.Wire(from: key([x.from]), fromPort: x.fromPort, to: key([x.to]), toPort: x.toPort))
    }
    let now = Date()
    for bird in presences where parts[ObjectIdentifier(bird)] == nil { bird.finishStep(now: now) }
    for id in order { let p = parts[id]!; p.bird.saved(written: p.cards, wires: p.wires, now: now) }
  }

  private func topConnections(_ project: JSONValue) -> [PresenceEvents.Connection] {
    (project["canvas"]?["connections"]?.array ?? []).compactMap(PresenceEvents.Connection.init)
  }
}

/// The helpers of a delegate call: a bird each, at the Cards it owns.
extension ProjectSession {
  /// The main Agent's bird and the helpers'.
  var presences: [AgentPresence] { [presence] + helperPresences }

  /// The bird of a helper (made when it first appears).
  func helperPresence(_ name: String) -> AgentPresence {
    if let p = helperPresences.first(where: { $0.label == name }) { return p }
    let p = AgentPresence()
    p.label = name
    p.tint = helperPresences.count + 1
    p.name = presence.name
    helperPresences.append(p)
    return p
  }

  /// A delegate call started: each helper's bird comes to its first Card.
  func startHelpers(_ tasks: [(name: String, cards: [String])]) {
    helperPresences.removeAll()
    cardHelper = [:]
    for t in tasks {
      for c in t.cards { cardHelper[c] = t.name }
      let p = helperPresence(t.name)
      p.turnStarted()
      if let first = t.cards.first, instance(key([first])) != nil { p.enqueue(.visit(key([first]))) }
    }
  }

  /// The bird that writes a Card: its helper's while one owns it.
  func presence(forCard k: CardKey) -> AgentPresence {
    let p = path(k)
    guard p.count == 1, let name = cardHelper[p[0]], let h = helperPresences.first(where: { $0.label == name }) else { return presence }
    return h
  }
}
