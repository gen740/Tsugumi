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
    for id in order { let p = parts[id]!; p.bird.saved(written: p.cards, wires: p.wires) }
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

/// The output frame beside a Card, used by explicit layout requests.
extension ProjectSession {
  /// Output frame of a Card (world units): saved place, or beside the Card.
  func displayRect(_ k: CardKey, origin: CGPoint? = nil) -> CGRect {
    let o = origin ?? position(k)
    let f = displayFrame(k)
    let sz = CGSize(width: f?["width"]?.double ?? 320, height: f?["height"]?.double ?? 170)
    let at = flow == .topToBottom
      ? CGPoint(x: f?["x"]?.double ?? (o.x + geometry(k).size.width + 40), y: f?["y"]?.double ?? o.y)
      : CGPoint(x: f?["x"]?.double ?? o.x, y: f?["y"]?.double ?? (o.y + geometry(k).height + 30))
    return CGRect(origin: at, size: sz)
  }

}
