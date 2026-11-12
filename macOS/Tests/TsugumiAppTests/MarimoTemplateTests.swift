import Foundation
import Testing
import TsugumiLauncher
import TsugumiRPC
@testable import TsugumiApp

@MainActor @Test func marimoTemplateCreatesEditableNotebookWithRetainedResult() throws {
  let dir = FileManager.default.temporaryDirectory.appendingPathComponent("marimo-\(UUID().uuidString)")
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: dir) }
  let document = #"{"name":"test","canvas":{"cards":[],"connections":[]}}"#
  try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  session.project = try JSONValue.parse(document)
  let card = session.addCard(template: .marimo, in: [], at: .zero)
  #expect(card == "marimo")
  #expect(session.kind(card) == .code)
  #expect(session.cardDefinition(card)?["reuseWorker"]?.bool == false)
  #expect(session.cardDefinition(card)?["artifactOutput"]?.string == "report")
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../..")
  let files = try JSONDecoder().decode([String: String].self, from: Data(contentsOf: root.appendingPathComponent("ui/templates/marimo.json")))
  #expect(files == MarimoTemplate.files)
  for file in ["src/main.py", "src/notebook.py", "default.nix", "_outputs.fbs"] {
    let bytes = try #require(session.sourceEdits["definitions/\(card)/\(file)"])
    #expect(String(decoding: bytes, as: UTF8.self) == files[file])
  }
}
