import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

@MainActor struct UntangleTests {
  @Test func portOrdersMatchWebAndPreserveNamedConnections() throws {
    for fixture in try ArrangeTests.fixture("untangle-contract.json") {
      let name = fixture["name"] as? String ?? ""
      let raw = try #require(fixture["cards"] as? [[String: Any]])
      let input = ArrangeTests.input(["cards": raw, "wires": fixture["wires"] ?? [], "flow": fixture["flow"] ?? ""])
      let cards = zip(input.cards, raw).map { c, j in
        UntangleCard(card: c, x: ArrangeTests.number(j["x"]), y: ArrangeTests.number(j["y"]), movable: j["movable"] as? Bool ?? false)
      }
      let orders = untanglePorts(flow: input.flow, cards: cards, wires: input.wires)
      let expected = (fixture["expect"] as? [[String: Any]] ?? []).map {
        PortOrder(id: $0["id"] as? String ?? "", inputs: $0["inputs"] as? [String] ?? [], outputs: $0["outputs"] as? [String] ?? [])
      }
      #expect(orders == expected, "\(name)")
      let after = cards.map { c in
        var c = c
        if let order = orders.first(where: { $0.id == c.card.id }) {
          for i in c.card.inputs.indices { c.card.inputs[i].name = order.inputs[i] }
          for i in c.card.outputs.indices { c.card.outputs[i].name = order.outputs[i] }
        }
        return c
      }
      #expect(untanglePorts(flow: input.flow, cards: after, wires: input.wires).isEmpty, "\(name): idempotent")
    }
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
  }
}
