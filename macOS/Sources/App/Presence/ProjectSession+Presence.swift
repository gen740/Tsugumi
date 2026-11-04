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

/// Tidying the canvas while the Agent works (LayoutTidy).
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

  /// The top level's blocks: each Card with the frame drawn at its default
  /// place after it, frames placed on their own, notes.
  func tidyBoxes() -> [LayoutTidy.Box] {
    var out: [LayoutTidy.Box] = []
    for id in cards(in: []).compactMap({ $0["id"]?.string }) {
      let k = key([id])
      let o = position(k)
      var block = geometry(k).outlineRect.offsetBy(dx: o.x, dy: o.y)
      if (displays[k] != nil || displayFrame(k) != nil) && displayFrame(k)?["hidden"]?.bool != true {
        let saved = displayFrame(k)?["x"]?.double != nil && displayFrame(k)?["y"]?.double != nil
        if saved { out.append(.init(id: "frame:" + id, rect: displayRect(k))) } else { block = block.union(displayRect(k)) }
      }
      out.append(.init(id: "card:" + id, rect: block))
    }
    for n in notes(in: []) {
      guard let id = n["id"]?.string else { continue }
      out.append(.init(id: "note:" + id, rect: CGRect(x: n["x"]?.double ?? 0, y: n["y"]?.double ?? 0,
                                                       width: n["width"]?.double ?? 240, height: n["height"]?.double ?? 120)))
    }
    return out
  }

  /// Makes room where blocks overlap (LayoutTidy), as one edit: saved like a
  /// drag, and undoable. Returns how many blocks moved.
  @discardableResult
  func tidy() -> Int {
    let moves = LayoutTidy.makeRoom(tidyBoxes(), across: flow == .leftToRight)
    guard !moves.isEmpty else { return 0 }
    func shift(_ v: JSONValue, _ by: CGVector) -> JSONValue {
      var v = v
      v["x"] = .real((((v["x"]?.double ?? 0) + by.dx) * 10).rounded() / 10)
      v["y"] = .real((((v["y"]?.double ?? 0) + by.dy) * 10).rounded() / 10)
      return v
    }
    edit { s in
      for m in moves {
        if m.id.hasPrefix("card:") {
          let k = key([String(m.id.dropFirst(5))])
          let p = s.position(k)
          s.setPosition(k, CGPoint(x: p.x + m.by.dx, y: p.y + m.by.dy))
        } else if m.id.hasPrefix("frame:") {
          let lk = s.layoutKey(key([String(m.id.dropFirst(6))]))
          s.updateLayout(of: []) { l in
            if let d = l["displays"]?[lk] { var all = l["displays"]!; all[lk] = shift(d, m.by); l["displays"] = all }
          }
        } else if m.id.hasPrefix("note:") {
          let id = String(m.id.dropFirst(5))
          s.updateLayout(of: []) { l in
            guard var notes = l["notes"]?.array, let i = notes.firstIndex(where: { $0["id"]?.string == id }) else { return }
            notes[i] = shift(notes[i], m.by)
            l["notes"] = .array(notes)
          }
        }
      }
    }
    return moves.count
  }
}
