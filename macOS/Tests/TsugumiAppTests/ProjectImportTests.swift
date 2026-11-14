import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiRPC

// Import now reads through the shared Host client. Run with the serialized
// service fixture so another suite cannot switch its state directory, and so
// the test never connects to a resident desktop service from another build.
extension ChatResumeTests {
  @Test func aCardsSummaryIsInItsMetaWhateverItsKind() async throws {
    try await withFixture { t in
      let s = withTestCardIds(t.session)
      let dir = s.identity.url, other = try SyncTests.project()
      defer { try? FileManager.default.removeItem(at: other) }
      let blob = Data([0, 255, 128])
      try blob.write(to: other.appendingPathComponent("definitions/a/.asset.bin"))
      s.start()
      try await SyncTests.connected(s)
      func written(_ id: String) throws -> String? {
        let url = dir.appendingPathComponent("definitions/\(id)/_meta.json")
        guard FileManager.default.fileExists(atPath: url.path) else { return nil }
        return try JSONValue.parse(String(decoding: Data(contentsOf: url), as: UTF8.self))["description"]?.string
      }

      let code = CardEditorModel(session: s, cardKey: "a")
      code.meta = .object([("description", .string("logs the value"))])
      #expect(await code.apply())
      #expect(try written("a") == "logs the value")
      #expect(s.nodeData("a").summary == "logs the value")
      #expect(s.cardDescription("a") == "logs the value")
      #expect(s.cardDefinition("a")?["description"] == nil)

      // A Card in Card has no _card.json, and its summary is saved all the same.
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
    }
  }
}
