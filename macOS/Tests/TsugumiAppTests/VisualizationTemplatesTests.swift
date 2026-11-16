import Testing
@testable import TsugumiApp

@Test func visualizationTemplatesAreOfferedByTheCanvas() {
  let templates = DefinitionTemplate.allCases.filter(\.isVisualization)
  #expect(templates.count == 3)
  #expect(templates.allSatisfy { BuiltinCardItems.visualizations.contains($0) })
}
