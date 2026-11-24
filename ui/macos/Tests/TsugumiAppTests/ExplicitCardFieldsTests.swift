import Foundation
import Testing
import TsugumiAgentClient
import TsugumiRPC
import TsugumiTestSupport

@testable import TsugumiApp

@Test func explicitCardFieldsAndWorkerBits() throws {
  let code = implementedCard(.code, symbol: CardKind.code.symbol, systems: ["aarch64-linux"])
  let executableFields = Set(["type", "symbol", "inputs", "maxRuns", "reuseWorker", "timeoutSeconds",
    "buildTimeoutSeconds", "recordLogs", "recordIO", "sandbox", "grants", "systems", "workerCapabilities"])
  #expect(Set(code.object?.map(\.0) ?? []) == executableFields)
  #expect(code["timeoutSeconds"]?.uint64 == 0)
  #expect(code["buildTimeoutSeconds"]?.uint64 == 0)
  #expect(code["sandbox"]?["limits"]?["cpu"]?.double == 0)
  #expect(code["sandbox"]?["limits"]?["memoryMiB"]?.uint64 == 0)
  #expect(code["recordLogs"]?.string == "project")
  #expect(code["recordIO"]?.string == "project")
  #expect(code["workerCapabilities"]?.array?.isEmpty == true)
  #expect(RecordSetting.allCases.map(\.rawValue) == ["project", "on", "off"])
  #expect(Set(implementedCard(.merge, symbol: "star", systems: []).object?.map(\.0) ?? []) ==
    Set(["type", "symbol", "inputs", "maxRuns", "recordIO"]))
  #expect(Set(implementedCard(.collect, symbol: "star", systems: []).object?.map(\.0) ?? []) ==
    Set(["type", "symbol", "maxRuns", "recordIO"]))
  for kind in [CardKind.trigger, .draft, .subgraph] {
    #expect(Set(implementedCard(kind, symbol: "star", systems: []).object?.map(\.0) ?? []) == Set(["type", "symbol"]))
  }

  let selected = WorkerBits.toggling(0, in: WorkerBits.toggling(63, in: []))
  #expect(WorkerBits.valid(selected))
  #expect(selected == [0, 63])
  #expect(WorkerBits.toggling(63, in: WorkerBits.toggling(0, in: selected)).isEmpty)
  #expect(!WorkerBits.valid([1, 1]))
  #expect(!WorkerBits.valid([64]))
}

@MainActor @Test func readmeTitleIsTheCardDisplayName() throws {
  let directory = testTemporaryDirectory.appendingPathComponent("title-\(UUID().uuidString.prefix(8))")
  try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: directory) }
  let session = try offlineSession(directory, cards: [("aggregate", fixtureCardJSON(.draft))])
  let readme = Documents.printReadme(title: "集計: Results", description: "", body: "Steps")
  #expect(readme == "---\ntitle: 集計: Results\ndescription: \n---\n\nSteps")
  let note = try #require(Documents.cardNotes(readme: readme, comments: nil))
  #expect(Documents.splitCardNotes(note).readme == readme)
  let definition = Tsugumi_RPC_DefinitionT()
  definition.id = "aggregate"
  definition.cardJson = fixtureCardJSON(.draft)
  definition.readme = readme
  let delta = Tsugumi_RPC_DocumentDeltaT()
  delta.baseRev = 0; delta.rev = 1; delta.definitionsUpsert = [definition]
  session.applyDelta(delta)
  #expect(session.displayName("aggregate") == "集計: Results")
  #expect(session.nodeData("aggregate").title == "集計: Results")
  #expect(session.definitionDir("aggregate") == "definitions/aggregate/")
}

@Test func cardAndPortNamesUseOneRule() {
  for name in ["a", "a_b2", String(repeating: "x", count: 64)] {
    #expect(validName(name))
  }
  for name in ["", "A", "1a", "_a", "a-b", ".", "a\n", "集計", String(repeating: "x", count: 65)] {
    #expect(!validName(name))
  }
  #expect(nextInputName(["in1", "in3"]) == "in2")
}

@Test func readmeAcceptsTheEmptyDescriptionHostdWrites() {
  let parsed = Documents.parseReadme("---\ntitle: plot\ndescription:\n---\n")
  #expect(parsed?.title == "plot")
  #expect(parsed?.description == "")
  #expect(Documents.parseReadme("---\ntitle: plot\ndescription:x\n---\n") == nil)
}
