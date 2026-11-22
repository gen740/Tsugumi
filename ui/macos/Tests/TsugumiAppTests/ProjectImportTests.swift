import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiRPC

// Import now reads through the shared Host client. Run with the serialized
// service fixture so another suite cannot switch its state directory, and so
// the test never connects to a resident desktop service from another build.
extension ChatResumeTests {
  @Test func aCardsSummaryIsInItsReadmeWhateverItsKind() async throws {
    try await withFixture { t in
      let s = withTestCardIds(t.session)
      let dir = s.identity.url, other = try SyncTests.project()
      defer { try? FileManager.default.removeItem(at: other) }
      let blob = Data([0, 255, 128])
      try blob.write(to: other.appendingPathComponent("definitions/a/.asset.bin"))
      let entered = AsyncStream<Void>.makeStream()
      let release = AsyncStream<Void>.makeStream()
      defer { release.continuation.finish(); entered.continuation.finish() }
      let place = s.prepareLayout
      s.prepareLayout = { project, layout, definitions in
        let result = try await place(project, layout, definitions)
        entered.continuation.yield(())
        for await _ in release.stream { }
        return result
      }
      s.start()
      try await SyncTests.connected(s)
      func written(_ id: String) throws -> String? {
        let url = dir.appendingPathComponent("definitions/\(id)/README.md")
        guard FileManager.default.fileExists(atPath: url.path) else { return nil }
        return Documents.parseReadme(try String(contentsOf: url, encoding: .utf8)).description
      }

      // The editor captures its base before the open's derivation completes.
      // Finish derivation deterministically before Apply: it cannot move that base.
      for await _ in entered.stream { break }
      let openedRev = s.rev
      let projectBytes = try Data(contentsOf: dir.appendingPathComponent("tsugumi.json"))
      let layoutBytes = try Data(contentsOf: dir.appendingPathComponent("layout.bin"))
      let code = CardEditorModel(session: s, cardKey: "a")
      code.meta = .object([("description", .string("logs the value"))])
      let second = try t.makeSession()
      second.start()
      try await SyncTests.connected(second)
      release.continuation.finish()
      await s.refreshDerivedLayout().value
      await second.refreshDerivedLayout().value
      #expect(s.rev == openedRev && second.rev == openedRev)
      #expect(s.position("a") == second.position("a"))
      #expect(!s.dirty && !second.dirty)
      #expect(try Data(contentsOf: dir.appendingPathComponent("tsugumi.json")) == projectBytes)
      #expect(try Data(contentsOf: dir.appendingPathComponent("layout.bin")) == layoutBytes)
      #expect(await code.apply())
      #expect(s.rev == openedRev + 1)
      #expect(s.saveError == nil)
      #expect(try written("a") == "logs the value")
      #expect(s.nodeData("a").summary == "logs the value")
      #expect(s.cardDescription("a") == "logs the value")
      #expect(s.cardDefinition("a")?["description"] == nil)

      // Saving the summary preserves the external reference in _card.json.
      let k = try await s.importProject(from: other, in: [], at: CGPoint(x: 400, y: 0))
      await s.save()
      #expect(s.saveError == nil)
      let name = path(k).last!
      #expect(try Data(contentsOf: dir.appendingPathComponent("externals/\(name)/definitions/a/.asset.bin")) == blob)
      let link = CardEditorModel(session: s, cardKey: k)
      link.meta = .object([("description", .string("the imported graph"))])
      #expect(await link.apply())
      #expect(try written(name) == "the imported graph")
      #expect(s.nodeData(k).summary == "the imported graph")
      #expect(s.cardExternal(k) == "externals/\(name)/")

      let child = try await s.addCard(template: .draft, in: [name], at: .zero)
      #expect(s.instance(child) != nil)
      let nested = dir.appendingPathComponent("externals/\(name)/definitions/\(path(child).last!)/_card.json")
      #expect(FileManager.default.fileExists(atPath: nested.path))
      await s.undo()
      #expect(s.instance(child) == nil && !FileManager.default.fileExists(atPath: nested.path))
    }
  }
}
