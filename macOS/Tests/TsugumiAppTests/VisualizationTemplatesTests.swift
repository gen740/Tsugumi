import Foundation
import Testing
import TsugumiLauncher
import TsugumiRPC
@testable import TsugumiApp

@MainActor @Test func visualizationTemplatesMakeEditableCardsWithSavedArtifacts() throws {
  let dir = FileManager.default.temporaryDirectory.appendingPathComponent("visualization-\(UUID().uuidString)")
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: dir) }
  let document = #"{"name":"test","canvas":{"cards":[],"connections":[]}}"#
  try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  let templates = DefinitionTemplate.allCases.filter { $0.visualizationLibrary != nil }
  #expect(templates.count == 9)
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../..")
  let shared = try JSONDecoder().decode([String: [String: String]].self, from: Data(contentsOf: root.appendingPathComponent("ui/templates/visualization.json")))
  #expect(VisualizationTemplates.files == shared)
  for template in templates {
    let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    session.project = try JSONValue.parse(document)
    let card = session.addCard(template: template, in: [], at: .zero)
    #expect(card == "visualization")
    #expect(session.kind(card) == .code)
    #expect(session.cardDefinition(card)?["retainArtifact"]?.bool == true)
    #expect(session.cardDefinition(card)?["artifactOutput"]?.string == "figure")
    for file in ["src/main.py", "default.nix", "_outputs.fbs"] {
      let bytes = try #require(session.sourceEdits["definitions/\(card)/\(file)"])
      #expect(String(decoding: bytes, as: UTF8.self) == shared[template.rawValue]?[file])
    }
  }
}
