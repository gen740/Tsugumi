import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

/// The native UI applies the core's proposals as one undoable layout edit.
@MainActor struct LayoutRPCTests {
  @Test func savedPortOrderIgnoresStaleAndDuplicateNames() {
    let data = [PortData(name: "a"), PortData(name: "b"), PortData(name: "new")]
    #expect(orderedPorts(data, saved: ["gone", "b", "b", "a"]).map(\.name) == ["b", "a", "new"])
  }

  @Test func untangleAndArrangePreserveConnectionsAndUndoInOneStep() async throws {
    let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("untangle-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let document = #"{"name":"untangle","canvas":{"cards":[],"connections":[]}}"#
    try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
    let daemon = try SyncTests.Daemon(project: dir)
    defer { daemon.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = daemon.endpoint
    s.start()
    try await SyncTests.connected(s)
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
    await s.untangle([])
    #expect(s.geometry("source").outputs == ["b", "a"])
    #expect(s.project == graph && s.definitionEdits == definitions)
    #expect(s.position("source") == CGPoint(x: 200, y: 0))
    #expect(s.isDraftWire(from: "source", to: "right"))
    #expect(!s.isDraftWire(from: "source", to: "left"))
    #expect(s.undoStack.count == steps + 1)
    s.undo()
    #expect(s.layout == before)
    #expect(s.geometry("source").outputs == ["a", "b"])
    await s.arrange([])
    #expect(s.undoStack.count == steps + 1)
    #expect(s.project == graph && s.definitionEdits == definitions)
    s.undo()
    #expect(s.layout == before)
    await s.moveSelection(cards: ["left"], notes: [], frames: [], by: CGSize(width: 1000, height: 0), snap: nil).value
    #expect(s.position("left") == CGPoint(x: 1000, y: 300))
    #expect(s.geometry("source").outputs == ["a", "b"])
    s.undo()
    #expect(s.layout == before)

    await s.untangle([])
    await s.moveSelection(cards: ["left"], notes: [], frames: [], by: CGSize(width: 1000, height: 0), snap: nil).value
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
