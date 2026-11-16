import Foundation
import Testing
import TsugumiLauncher
import TsugumiRPC
import TsugumiTestSupport
@testable import TsugumiApp

@MainActor @Test func marimoTemplateCreatesEditableNotebookWithRetainedResult() async throws {
  let dir = testTemporaryDirectory.appendingPathComponent("marimo-\(UUID().uuidString)")
  defer { try? FileManager.default.removeItem(at: dir) }
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  let document = #"{"name":"test","canvas":{"cards":[],"connections":[]}}"#
  try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  let session = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
  session.project = try JSONValue.parse(document)
  let card = try await session.addCard(template: .marimo, in: [], at: .zero)
  #expect(card == "marimo")
  #expect(session.kind(card) == .code)
  #expect(session.sourceEdits["definitions/\(card)/src/notebook.py"]?.isEmpty == false)
  let beforeUndo = session.docState()
  await session.undo()
  #expect(session.cardDefinition(card) == nil)
  await session.redo()
  #expect(session.docState() == beforeUndo)
}
