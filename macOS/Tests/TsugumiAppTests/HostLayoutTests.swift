import Foundation
import Testing
import TsugumiTestSupport
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

/// The native UI applies the host's proposals as one undoable layout edit.
@MainActor @Suite(.hostService) struct HostLayoutTests {
  @Test func camerasAreCheckedBeforeSavingAndAfterLoading() throws {
    let valid = ClientViewState.Camera(scope: [], x: 1.5, y: -2, zoom: 1.25)
    let bad = [0.0, -1, .infinity, .nan].map { ClientViewState.Camera(scope: [], x: 0, y: 0, zoom: $0) }
    let view = ClientViewState(scope: [], cameras: [valid] + bad, selectedCards: [["a"]])
    let decoded = try JSONDecoder().decode(ClientViewState.self, from: Data(try view.json().utf8))
    #expect(decoded.cameras.count == 1)
    #expect(decoded.cameras[0].zoom == 1.25)
    #expect(!ClientViewState.Camera(scope: [], x: .infinity, y: 0, zoom: 1).valid)
  }

  @Test func savedPortOrderIgnoresStaleAndDuplicateNames() {
    let data = [PortData(name: "a"), PortData(name: "b"), PortData(name: "new")]
    #expect(orderedPorts(data, saved: ["gone", "b", "b", "a"]).map(\.name) == ["b", "a", "new"])
  }

  @Test func untangleAndArrangePreserveConnectionsAndUndoInOneStep() async throws {
    let dir = testTemporaryDirectory.appendingPathComponent("untangle-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: dir) }
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let document = #"{"name":"untangle","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}}"#
    try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
    let daemon = try SyncTests.Daemon(project: dir)
    defer { daemon.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = daemon.endpoint
    s.start()
    try await SyncTests.connected(s)
    s.layout = try JSONValue.parse(#"{"flow":"topToBottom"}"#)
    for (id, x, y, ins, outs) in [("source", 200.0, 0.0, [], ["a", "b"]), ("left", 0, 300, ["in"], []), ("right", 500, 300, ["in"], [])] {
      _ = try await s.addCard(template: .python, in: [], at: CGPoint(x: x, y: y), name: id)
      func ports(_ names: [String]) -> JSONValue { .array(names.map { .object([("name", .string($0))]) }) }
      s.updateCard(id) { $0["inputs"] = ports(ins) }
      if !outs.isEmpty {
        s.putSource("definitions/\(id)/_outputs.fbs", "namespace layout; table Value { value:double; } table Outputs { " + outs.map { "\($0):Value;" }.joined(separator: " ") + " } root_type Outputs;")
      }
    }
    try await s.connect(from: ("source", "a"), to: ("right", "in"), in: [])
    try await s.connect(from: ("source", "b"), to: ("left", "in"), in: [])
    // The daemon keeps the saved Cards, so measure from the state it has taken in.
    try await waitForModel { !(s.dirty || s.saving) }
    #expect(!s.dirty, "\(s.saveError ?? "")")
    let note = try await s.addNote(in: [], at: CGPoint(x: 3, y: 4))
    s.updateDisplayFrame("left") { $0["width"] = .real(300); $0["height"] = .real(200) }
    let before = s.layout, graph = s.project, definitions = s.definitionEdits, steps = s.undoStack.count
    await s.untangle([])
    #expect(s.geometry("source").outputs == ["b", "a"])
    #expect(s.project == graph && s.definitionEdits == definitions)
    #expect(s.position("source") == CGPoint(x: 200, y: 0))
    #expect(s.undoStack.count == steps + 1)
    await s.undo()
    #expect(s.layout == before)
    #expect(s.geometry("source").outputs == ["a", "b"])
    await s.arrange([])
    #expect(s.undoStack.count == steps + 1)
    #expect(s.project == graph && s.definitionEdits == definitions)
    #expect(s.displayFrame("left")?["width"]?.double == 300)
    #expect(s.displayFrame("left")?["x"]?.double != nil)
    #expect(s.note(note)?["x"]?.double != 3 || s.note(note)?["y"]?.double != 4)
    await s.undo()
    #expect(s.layout == before)
    let was = s.position("left"), other = s.position("source"), frame = s.displayFrame("left")
    await s.moveSelection(cards: ["left"], notes: [], frames: [], by: CGSize(width: 47, height: 33), snap: 20).value
    #expect(s.position("left") == CGPoint(x: was.x + 40, y: was.y + 40))
    #expect(s.position("source") == other && s.displayFrame("left") == frame)
    await s.undo()
    #expect(s.layout == before)
    await s.moveSelection(cards: ["left"], notes: [], frames: [], by: CGSize(width: 1000, height: 0), snap: nil).value
    #expect(s.position("left") == CGPoint(x: 1000, y: 300))
    #expect(s.geometry("source").outputs == ["a", "b"])
    await s.undo()
    #expect(s.layout == before)

    await s.untangle([])
    await s.moveSelection(cards: ["left"], notes: [], frames: [], by: CGSize(width: 1000, height: 0), snap: nil).value
    #expect(s.geometry("source").outputs == ["a", "b"])
    await s.undo()
    #expect(s.geometry("source").outputs == ["b", "a"])
    #expect(s.hasPortOrder("source"))
    await s.resetPortOrder(["source"]).value
    #expect(s.geometry("source").outputs == ["a", "b"])
    #expect(!s.hasPortOrder("source"))
    await s.undo()
    #expect(s.geometry("source").outputs == ["b", "a"])
    await s.close()
  }
}
