import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiRPC

@MainActor struct RunComparisonTests {
  @Test func parametersAlignByNameAndRetainMissingSides() {
    func record(_ json: String) -> Tsugumi_RPC_RecordResultT {
      let result = Tsugumi_RPC_RecordResultT()
      result.record = Tsugumi_RPC_InvocationRecordT()
      result.record?.paramsJson = json
      return result
    }
    let a = record(#"{"low":600,"boundary":1650}"#), b = record(#"{"boundary":1800,"low":600,"added":true}"#)
    let fields = RunComparison.params(a, b)
    #expect(fields.map(\.name) == ["added", "boundary", "low"])
    #expect(fields.map(\.changed) == [true, true, false])
    #expect(fields[0].left == nil)
    #expect(fields[1].left == "1650" && fields[1].right == "1800")
    #expect(a.record?.paramsJson == #"{"low":600,"boundary":1650}"#)
  }

  @Test func retainedDisplayEqualityIgnoresIdentityButChecksContent() {
    let first = DisplaySnapshot(runId: "left", invocation: 1, revision: 1, value: .table(columns: ["Band", "Photons"], rows: [["red", "101187"]]))
    let equal = DisplaySnapshot(runId: "right", invocation: 3, revision: 9, value: first.value)
    let changed = DisplaySnapshot(runId: "right", invocation: 3, revision: 9, value: .table(columns: ["Band", "Photons"], rows: [["red", "104821"]]))
    #expect(RunComparison.sameDisplay(first, equal))
    #expect(!RunComparison.sameDisplay(first, changed))
    #expect(!RunComparison.sameDisplay(first, nil))
    let image = DisplaySnapshot(runId: "left", invocation: 1, revision: 1, value: .image(mediaType: "image/png", data: Data([1, 2]), alt: "rgb"))
    let other = DisplaySnapshot(runId: "right", invocation: 1, revision: 1, value: .image(mediaType: "image/png", data: Data([1, 3]), alt: "rgb"))
    #expect(!RunComparison.sameDisplay(image, other))
  }

  @Test func recordedDisplayDoesNotNeedALiveSession() {
    let value = Tsugumi_Data_DisplayTableT()
    value.columns = ["Band", "Photons"]
    let row = Tsugumi_Data_TableRowT()
    row.cells = ["red", "101187"]
    value.rows = [row]
    let display = Tsugumi_Data_DisplayT()
    display.runId = "old-run"
    display.invocation = 7
    display.value = Tsugumi_Data_DisplayValueUnion(value, type: .displaytable)
    let decoded = recordedDisplay(display)
    #expect(decoded?.runId == "old-run" && decoded?.invocation == 7)
    guard case .table(let columns, let rows) = decoded?.value else { Issue.record("expected a retained table"); return }
    #expect(columns == ["Band", "Photons"] && rows == [["red", "101187"]])
  }

  @Test func retainedCopyIdentityDoesNotChangeTheRecordedValue() {
    func record(_ id: String, _ bytes: [UInt8]) -> Tsugumi_RPC_RecordResultT {
      let value = Tsugumi_Data_NamedValueT()
      value.name = "photons"; value.rootType = "Counts"; value.inlineBytes = bytes; value.artifactId = id
      let result = Tsugumi_RPC_RecordResultT()
      result.record = Tsugumi_RPC_InvocationRecordT()
      result.record?.outputs = [value]
      return result
    }
    let a = record("copy-left", [1, 2]), b = record("copy-right", [1, 2])
    #expect(RunComparison.values(true, a, b).first?.changed == false)
    #expect(a.record?.outputs.first??.artifactId == "copy-left")
    b.record?.outputs.first??.inlineBytes = [1, 3]
    #expect(RunComparison.values(true, a, b).first?.changed == true)
  }
}
