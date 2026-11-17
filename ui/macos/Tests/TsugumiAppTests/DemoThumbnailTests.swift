import Foundation
import SwiftUI
import Testing
@testable import TsugumiApp

private func demoIds() throws -> [String] {
  var repo = URL(fileURLWithPath: #filePath)
  for _ in 0..<5 { repo.deleteLastPathComponent() }
  let index = try JSONSerialization.jsonObject(with: Data(contentsOf: repo.appendingPathComponent("demos/ja/index.json"))) as? [String: Any]
  return (index?["demos"] as? [[String: Any]] ?? []).compactMap { $0["id"] as? String }
}

private func scene(_ id: String) throws -> DemoScene {
  let root = try #require(DemoPlayerData.root)
  let bytes = try Data(contentsOf: root.appendingPathComponent("\(id).json"))
  return DemoScene(try JSONDecoder().decode(DemoPlayerData.self, from: bytes), images: root)
}

/// Every demo of Home has the site's data: wires name real ports, or attach
/// directly to a Draft with an empty port name, and its images are there.
@Test func everyDemoHasAThumbnail() throws {
  let ids = try demoIds()
  #expect(!ids.isEmpty)
  for id in ids {
    let s = try scene(id)
    #expect(!s.placed.isEmpty, "\(id) has no Cards")
    let cards = Dictionary(s.placed.map { ($0.card.id, $0.card) }, uniquingKeysWith: { a, _ in a })
    for c in s.data.cards where c.kind == "draft" {
      #expect(c.inputs.isEmpty && c.outputs.isEmpty, "\(id): \(c.id) has no ports")
    }
    for w in s.data.wires {
      let from = try #require(cards[w.from]), to = try #require(cards[w.to])
      #expect(from.kind == "draft" ? w.fromPort.isEmpty : !w.fromPort.isEmpty && from.outputs.contains { $0.name == w.fromPort }, "\(id): \(w.from).\(w.fromPort)")
      #expect(to.kind == "draft" ? w.toPort.isEmpty : !w.toPort.isEmpty && to.inputs.contains { $0.name == w.toPort }, "\(id): \(w.to).\(w.toPort)")
    }
    for shown in s.shown {
      if case .image = shown.output { #expect(shown.image != nil, "\(id): the image of \(shown.id)") }
    }
  }
}

/// The real Draft demos use Card attachments in both flow directions. Check
/// the drawn path, so accepting an empty port cannot hide a misplaced wire.
@Test func thumbnailWiresMeetDraftCards() throws {
  for id in try demoIds() {
    let original = try scene(id).data
    guard original.cards.contains(where: { $0.kind == "draft" }) else { continue }
    for flow in ["topToBottom", "leftToRight"] {
      var data = original
      data.flow = flow
      let s = DemoScene(data)
      let cards = Dictionary(uniqueKeysWithValues: s.placed.map { ($0.card.id, $0) })
      #expect(s.wires.count == data.wires.count)
      for w in s.wires {
        var start: CGPoint?, end: CGPoint?
        w.path.forEach { element in
          switch element {
          case .move(to: let p): if start == nil { start = p }; end = p
          case .line(to: let p), .quadCurve(to: let p, control: _), .curve(to: let p, control1: _, control2: _): end = p
          case .closeSubpath: break
          }
        }
        for (id, output, point) in [(w.wire.from, true, start), (w.wire.to, false, end)] {
          let p = try #require(cards[id])
          guard p.card.kind == "draft" else { continue }
          let r = p.outline
          let expected = flow == "topToBottom"
            ? CGPoint(x: r.midX, y: output ? r.maxY : r.minY)
            : CGPoint(x: output ? r.maxX : r.minX, y: r.midY)
          #expect(try #require(point) == expected, "\(id): \(flow), output=\(output)")
          #expect(w.dotted)
        }
      }
    }
  }
}

/// The reason for drawing them this way: a left-to-right demo's wires leave a
/// Card's right side and enter the next one's left side.
@Test func aThumbnailFollowsTheFlow() throws {
  let casa = try scene("casa")
  #expect(casa.flow == .leftToRight)
  let rects = Dictionary(casa.placed.map { ($0.card.id, $0.rect) }, uniquingKeysWith: { a, _ in a })
  for w in casa.wires {
    let box = w.path.boundingRect
    let from = try #require(rects[w.wire.from]), to = try #require(rects[w.wire.to])
    #expect(abs(box.minX - from.maxX) < 40, "\(w.wire.from) → \(w.wire.to)")
    #expect(abs(box.maxX - to.minX) < 40, "\(w.wire.from) → \(w.wire.to)")
  }
  #expect(!casa.shown.isEmpty, "its results are in the thumbnail")
}

/// The replay goes through every step in order and ends as the run ended; the
/// results appear only once their Card has run.
@Test func theReplayEndsWhereTheRunEnded() throws {
  let s = try scene("casa")
  #expect(s.runnable)
  let frames = s.frames(speed: 1)
  #expect(frames.map(\.step) == frames.map(\.step).sorted())
  #expect(Set(frames.map(\.step)).count == s.data.steps.count)
  for shown in s.shown {
    #expect(!s.isShown(shown, at: .idle))
    #expect(s.isShown(shown, at: .done))
  }
  for p in s.placed {
    #expect(s.node(p, at: .idle).state == nil)
    if s.data.final[p.card.id] != nil { #expect(s.node(p, at: .done).state == .succeeded, "\(p.card.id)") }
  }
  for w in s.data.wires where s.data.final[w.from]?.sent.contains(w.fromPort) == true {
    #expect(s.style(w, at: .idle) == .normal)
    #expect(s.style(w, at: .done) == .carried)
  }
  // A value on its way moves at some moment of the replay.
  #expect(frames.contains { f in s.data.wires.contains { s.style($0, at: f) == .active } })
}

/// A run moves nothing: a Card has the same size before, during and after it.
@Test func aRunMovesNothing() throws {
  let s = try scene("collatz")
  for p in s.placed {
    for f in s.frames(speed: 1) + [.idle, .done] {
      #expect(CardGeometry(s.node(p, at: f), flow: s.flow).size == p.rect.size, "\(p.card.id)")
    }
  }
}

/// No two of a demo's Cards lie on each other, in Home's thumbnails as on the
/// Canvas: both use this geometry. ui/web/tests/thumbnails.ts checks the Web UI's.
@Test func noThumbnailHasCardsOnEachOther() throws {
  for id in try demoIds() {
    let s = try scene(id)
    for (i, a) in s.placed.enumerated() {
      for b in s.placed[(i + 1)...] {
        #expect(!a.rect.intersects(b.rect), "\(id): \(a.card.id) \(a.rect) lies on \(b.card.id) \(b.rect)")
      }
    }
  }
}

/// A Draft has no run to play.
@Test func aDraftHasNoReplay() throws {
  let s = try scene("casa-draft")
  #expect(!s.runnable)
}
