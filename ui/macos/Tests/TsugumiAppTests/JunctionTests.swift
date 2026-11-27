import FlatBuffers
import Foundation
import Testing
import TsugumiLauncher
import TsugumiRPC
import TsugumiTestSupport
@testable import TsugumiApp

/// A junction is where one output's wires split on the canvas: layout only.
/// The host's Arrange places them (tests/core/host/project_operations_test.cc);
/// here the app's side: which ones are drawn, and the layout edits for them.
@MainActor struct JunctionTests {
  /// `s.out` feeds three inputs, `p.out` two.
  nonisolated private static let project = fixtureProjectJSON(#"""
    {"canvas":{"cards":[{"id":"s"},{"id":"a"},{"id":"b"},{"id":"c"},{"id":"p"},{"id":"q"},{"id":"r"}],"connections":[
    {"from":{"card":"s","port":"out"},"to":{"card":"a","port":"in"}},
    {"from":{"card":"s","port":"out"},"to":{"card":"b","port":"in"}},
    {"from":{"card":"s","port":"out"},"to":{"card":"c","port":"in"}},
    {"from":{"card":"p","port":"out"},"to":{"card":"q","port":"in"}},
    {"from":{"card":"p","port":"out"},"to":{"card":"r","port":"in"}}]}}
    """#)
  nonisolated private static let placed = #"""
    {"flow":"topToBottom","cards":{"s":{"x":0,"y":0},"a":{"x":-600,"y":600},"b":{"x":0,"y":600},"c":{"x":600,"y":900},
    "p":{"x":2000,"y":0},"q":{"x":2000,"y":600},"r":{"x":2400,"y":600}}}
    """#

  private func session(layout: String = placed) throws -> (ProjectSession, URL) {
    let dir = testTemporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    try Data(Self.project.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
    let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    session.prepareLayout = { _, layout, _ in layout }
    let snapshot = Tsugumi_RPC_ProjectSnapshotT()
    snapshot.rev = 0; snapshot.projectJson = Self.project
    snapshot.layout = fixtureLayout(layout)
    snapshot.definitions = ["s", "a", "b", "c", "p", "q", "r"].map { id in
      let definition = Tsugumi_RPC_DefinitionT(); definition.id = id; definition.cardJson = fixtureCardJSON(.collect)
      definition.readme = Documents.printReadme(title: id, description: "", body: "")
      return definition
    }
    snapshot.run = Tsugumi_RPC_RunStatusT()
    session.receiveProjectSnapshot(snapshot, projectId: "p")
    return (session, dir)
  }

  /// The host's reply: the layout it saved, the project as it was.
  private func reply(_ s: ProjectSession, _ request: Tsugumi_Host_ProjectOperateRequestT, layout: String) -> Tsugumi_RPC_SaveResultT {
    let delta = Tsugumi_RPC_DocumentDeltaT()
    delta.baseRev = s.rev; delta.rev = s.rev + 1; delta.opId = request.opId; delta.origin = "ui"
    delta.layoutChanged = true; delta.layout = fixtureLayout(layout)
    var builder = FlatBufferBuilder(initialSize: 64)
    var inverse: Tsugumi_Host_ProjectEditsT? = Tsugumi_Host_ProjectEditsT()
    builder.finish(offset: Tsugumi_Host_ProjectEdits.pack(&builder, obj: &inverse))
    let result = Tsugumi_RPC_SaveResultT()
    result.rev = delta.rev; result.delta = delta; result.operationData = Array(builder.data)
    return result
  }

  private func withJunction(_ port: String, x: Double, y: Double, on card: String = "s") -> String {
    var layout = try! JSONValue.parse(Self.placed)
    layout["cards"]?[card]?["junctions"] = .array([.object([("port", .string(port)), ("x", .real(x)), ("y", .real(y))])])
    return layout.serialized()
  }

  @Test func outputsWithTwoOrMoreInputsBranchAtAJunction() throws {
    var layout = try JSONValue.parse(withJunction("out", x: 100, y: 300))
    layout["cards"]?["p"]?["junctions"] = .array([.object([("port", .string("out")), ("x", .real(2100)), ("y", .real(300))])])
    layout["cards"]?["a"]?["junctions"] = .array([.object([("port", .string("out")), ("x", .real(-500)), ("y", .real(900))])])
    let (s, dir) = try session(layout: layout.serialized())
    defer { try? FileManager.default.removeItem(at: dir) }
    #expect(s.junctions(in: []) == [ProjectSession.JunctionPlace(card: "s", port: "out", at: CGPoint(x: 100, y: 300)),
                                    ProjectSession.JunctionPlace(card: "p", port: "out", at: CGPoint(x: 2100, y: 300))])
    #expect(s.junction("p", "out") == CGPoint(x: 2100, y: 300), "one placed by hand on two inputs is drawn")
    #expect(s.junction("a", "out") == nil, "without inputs there is nothing to branch")
    #expect(s.junctionOutputs("s") == ["out"] && s.junctionOutputs("p") == ["out"] && s.junctionOutputs("a").isEmpty)
    #expect(s.fanout("s", "out") == 3 && s.fanout("p", "out") == 2)
    // layout.bin keeps them as they were saved.
    #expect(Documents.layoutView(fixtureLayout(layout.serialized()), notes: [:])["cards"]?["p"]?["junctions"] == layout["cards"]?["p"]?["junctions"])
  }

  @Test func addingMovingAndRemovingAreLayoutEditsOnly() async throws {
    let (s, dir) = try session()
    defer { try? FileManager.default.removeItem(at: dir) }
    let connections = s.connections(in: []).map { $0.serialized() }
    var saved: [Tsugumi_Host_ProjectFieldEditT] = []
    var next = Self.placed
    s.operateProject = { request in
      let edits = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
      for edit in edits.edits { saved.append(try #require(edit?.value?.value as? Tsugumi_Host_ProjectFieldEditT)) }
      return self.reply(s, request, layout: next)
    }
    // Added from the Card's menu: where Arrange would put it, below the output.
    let source = s.placedPosition("s").y + s.geometry("s").port("out", output: true).y
    s.addJunction("s", "out")
    await s.save()
    let add = try #require(saved.last)
    #expect(add.filePath == "layout.bin")
    #expect(add.selector.compactMap { $0?.field } == ["cards", "s", "junctions"])
    #expect(try JSONValue.parse(try #require(add.selector.last??.matchJson)) == .object([("port", .string("out"))]))
    #expect(!add.oldValue.present && add.newValue.present)
    let value = try JSONValue.parse(try #require(add.newValue.json))
    let y = try #require(value["y"]?.double)
    #expect(value["port"]?.string == "out" && y > source && y < 600)

    // Dragged: the same entry, moved.
    next = withJunction("out", x: 40, y: 320)
    s.setJunction("s", "out", at: CGPoint(x: 40, y: 320))
    await s.save()
    #expect(s.junction("s", "out") == CGPoint(x: 40, y: 320))

    // Removed: its wires go directly again.
    next = Self.placed
    s.setJunction("s", "out", at: nil)
    await s.save()
    let remove = try #require(saved.last)
    #expect(remove.oldValue.present && !remove.newValue.present)
    #expect(s.junction("s", "out") == nil)
    #expect(saved.allSatisfy { $0.filePath == "layout.bin" }, "never the project's connections")
    #expect(s.connections(in: []).map { $0.serialized() } == connections)
  }

  @Test func arrangeAsksTheHostThenShowsTheArrangedGraph() async throws {
    let (s, dir) = try session()
    defer { try? FileManager.default.removeItem(at: dir) }
    s.operateProject = { request in
      let layout = try #require(request.body?.value as? Tsugumi_Host_ProjectLayoutT)
      #expect(layout.action == .arrange)
      return self.reply(s, request, layout: self.withJunction("out", x: 60, y: 360))
    }
    final class Scopes: @unchecked Sendable { var list: [[String]] = [] }
    let arranged = Scopes()
    let observer = NotificationCenter.default.addObserver(forName: .canvasArranged, object: s, queue: nil) { n in
      arranged.list.append(n.userInfo?["scope"] as? [String] ?? ["?"])
    }
    defer { NotificationCenter.default.removeObserver(observer) }
    await s.arrange([])
    #expect(arranged.list == [[]], "the canvases of the scope fit the arranged graph")
    #expect(s.junction("s", "out") == CGPoint(x: 60, y: 360))
  }
}
