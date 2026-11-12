import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

@MainActor struct UntangleTests {
  @Test func portOrdersReduceCrossingsAndPreserveNamedConnections() throws {
    for fixture in try ArrangeTests.fixture("untangle-contract.json") {
      let name = fixture["name"] as? String ?? ""
      let raw = try #require(fixture["cards"] as? [[String: Any]])
      let input = ArrangeTests.input(["cards": raw, "wires": fixture["wires"] ?? [], "flow": fixture["flow"] ?? ""])
      let cards = zip(input.cards, raw).map { c, j in
        UntangleCard(card: c, x: ArrangeTests.number(j["x"]), y: ArrangeTests.number(j["y"]), movable: j["movable"] as? Bool ?? false)
      }
      let extra = (fixture["extra"] as? [[String: Any]] ?? []).map {
        CGRect(x: ArrangeTests.number($0["x"]), y: ArrangeTests.number($0["y"]),
               width: ArrangeTests.number($0["width"]), height: ArrangeTests.number($0["height"]))
      }
      let orders = untanglePorts(flow: input.flow, cards: cards, wires: input.wires, extra: extra)
      let after = cards.map { c in
        var c = c
        if let order = orders.first(where: { $0.id == c.card.id }) {
          for i in c.card.inputs.indices { c.card.inputs[i].name = order.inputs[i] }
          for i in c.card.outputs.indices { c.card.outputs[i].name = order.outputs[i] }
        }
        return c
      }
      let original = try crossings(cards, input: input, extra: extra)
      let improved = try crossings(after, input: input, extra: extra)
      #expect(orders.isEmpty ? improved == original : improved < original, "\(name)")
      if name == "adjacent outputs to Draft and code" {
        #expect(original == 1 && improved == 0)
      }
      #expect(untanglePorts(flow: input.flow, cards: after, wires: input.wires, extra: extra).isEmpty, "\(name): idempotent")
    }
  }

  private func crossings(_ cards: [UntangleCard], input: ArrangeInput, extra: [CGRect]) throws -> Int {
    let ends = try input.wires.map { w in
      let from = try #require(cards.first { $0.card.id == w.from })
      let to = try #require(cards.first { $0.card.id == w.to })
      let output = try #require(from.card.outputs.first { $0.name == w.fromPort })
      let input = try #require(to.card.inputs.first { $0.name == w.toPort })
      return (a: CGPoint(x: from.x + output.x, y: from.y + output.y),
              b: CGPoint(x: to.x + input.x, y: to.y + input.y))
    }
    let obstacles = cards.map { CGRect(x: $0.x, y: $0.y, width: $0.card.width, height: $0.card.height) } + extra
    var router = WireRouter(flow: input.flow, obstacles: obstacles)
    let corners = router.plan(ends)
    return crossingPairs(ends.indices.map { corners[$0] ?? router.curvePoints(ends[$0].a, ends[$0].b) }, ends: ends)
  }

  @Test func stalePortNamesCannotDuplicateOrHideCurrentPorts() {
    let data = [PortData(name: "a"), PortData(name: "b"), PortData(name: "new")]
    #expect(orderedPorts(data, saved: ["gone", "b", "b", "a"]).map(\.name) == ["b", "a", "new"])
  }

  @Test func untangleAndArrangePreserveConnectionsAndUndoInOneStep() throws {
    let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("untangle-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    let document = #"{"name":"untangle","canvas":{"cards":[],"connections":[]}}"#
    try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.project = try JSONValue.parse(document)
    s.layout = try JSONValue.parse(#"{"flow":"topToBottom"}"#)
    for (id, x, y, ins, outs) in [("source", 200.0, 0.0, [], ["a", "b"]), ("left", 0, 300, ["in"], []), ("right", 500, 300, ["in"], [])] {
      _ = s.addCard(template: id == "right" ? .draft : .python, in: [], at: CGPoint(x: x, y: y), name: id)
      func ports(_ names: [String]) -> JSONValue { .array(names.map { .object([("name", .string($0))]) }) }
      s.updateCard(id) { $0["inputs"] = ports(ins) }
      s.setDefinition(id, card: s.cardDefinition(id)!, meta: .object([("outputs", ports(outs))]))
    }
    s.connect(from: ("source", "a"), to: ("right", "in"), in: [])
    s.connect(from: ("source", "b"), to: ("left", "in"), in: [])
    let before = s.layout, graph = s.project, definitions = s.definitionEdits, steps = s.undoStack.count
    s.untangle([])
    #expect(s.geometry("source").outputs == ["b", "a"])
    #expect(s.project == graph && s.definitionEdits == definitions)
    #expect(s.position("source") == CGPoint(x: 200, y: 0))
    #expect(s.isDraftWire(from: "source", to: "right"))
    #expect(!s.isDraftWire(from: "source", to: "left"))
    #expect(s.undoStack.count == steps + 1)
    s.undo()
    #expect(s.layout == before)
    #expect(s.geometry("source").outputs == ["a", "b"])
    s.arrange([])
    #expect(s.undoStack.count == steps + 1)
    #expect(s.project == graph && s.definitionEdits == definitions)
    s.undo()
    #expect(s.layout == before)
    s.moveSelection(cards: ["left"], notes: [], frames: [], by: CGSize(width: 1000, height: 0), snap: nil)
    #expect(s.position("left") == CGPoint(x: 1000, y: 300))
    #expect(s.geometry("source").outputs == ["a", "b"])
    s.undo()
    #expect(s.layout == before)

    s.untangle([])
    s.moveSelection(cards: ["left"], notes: [], frames: [], by: CGSize(width: 1000, height: 0), snap: nil)
    #expect(s.geometry("source").outputs == ["a", "b"])
    s.undo()
    #expect(s.geometry("source").outputs == ["b", "a"])
    #expect(s.hasPortOrder("source"))
    s.resetPortOrder(["source"])
    #expect(s.geometry("source").outputs == ["a", "b"])
    #expect(!s.hasPortOrder("source"))
    s.undo()
    #expect(s.geometry("source").outputs == ["b", "a"])
  }
}
