import Testing
@testable import TsugumiApp

@Test func visualizationTemplatesAreOfferedByTheCanvas() {
  let templates = DefinitionTemplate.allCases.filter(\.isVisualization)
  #expect(!templates.isEmpty)
  #expect(Set(templates) == Set(BuiltinCardItems.visualizations))
}
