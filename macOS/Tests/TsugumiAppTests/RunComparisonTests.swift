import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiRPC

@MainActor struct RunComparisonTests {
  @Test func parametersAlignByNameAndRetainMissingSides() {
    func record(_ json: String) -> ComparedRunCard {
      let result = Tsugumi_RPC_RecordResultT()
      result.record = Tsugumi_RPC_InvocationRecordT()
      result.record?.paramsJson = json
      return ComparedRunCard(summary: Tsugumi_RPC_InvocationSummaryT(), record: result)
    }
    let a = record(#"{"low":600,"boundary":1650}"#), b = record(#"{"boundary":1800,"low":600,"added":true}"#)
    let fields = RunComparison.params(a, b)
    #expect(fields.map(\.name) == ["added", "boundary", "low"])
    #expect(fields.map(\.changed) == [true, true, false])
    #expect(fields[0].left == nil)
    #expect(fields[1].left == "1650" && fields[1].right == "1800")
    #expect(a.record?.record?.paramsJson == #"{"low":600,"boundary":1650}"#)
  }

  @Test func executionServiceAndMethodComeFromTheInvocation() {
    let local = Tsugumi_RPC_InvocationSummaryT()
    local.executionService = "local"
    let emulated = Tsugumi_RPC_InvocationSummaryT()
    emulated.executionService = "local"
    emulated.emulator = "qemu-x86_64"
    let old = Tsugumi_RPC_InvocationSummaryT()
    let a = ComparedRunCard(summary: local, record: nil)
    let b = ComparedRunCard(summary: emulated, record: nil)
    let older = ComparedRunCard(summary: old, record: nil)

    let fields = RunComparison.conditions(a, b)
    #expect(fields[1].difference == .changed)
    #expect(fields[2].difference == .same)
    #expect(fields[2].left != nil)

    let missing = RunComparison.conditions(a, older)
    #expect(missing[1].difference == .unknown && missing[1].right == nil)
    #expect(missing[2].difference == .unknown && missing[2].right == nil)
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
    func record(_ id: String, _ bytes: [UInt8]) -> ComparedRunCard {
      let value = Tsugumi_Data_NamedValueT()
      value.name = "photons"; value.rootType = "Counts"; value.inlineBytes = bytes; value.artifactId = id
      let result = Tsugumi_RPC_RecordResultT()
      result.record = Tsugumi_RPC_InvocationRecordT()
      result.record?.outputs = [value]
      return ComparedRunCard(summary: Tsugumi_RPC_InvocationSummaryT(), record: result)
    }
    let a = record("copy-left", [1, 2]), b = record("copy-right", [1, 2])
    #expect(RunComparison.values(true, a, b).first?.changed == false)
    #expect(a.record?.record?.outputs.first??.artifactId == "copy-left")
    b.record?.record?.outputs.first??.inlineBytes = [1, 3]
    #expect(RunComparison.values(true, a, b).first?.changed == true)
  }
  @Test func omittedValuesCompareByContentHashAndType() {
    let a = Tsugumi_Data_NamedValueT(), b = Tsugumi_Data_NamedValueT()
    a.sizeBytes = 4096; b.sizeBytes = 4096
    a.rootType = "Counts"; b.rootType = "Counts"
    #expect(RunComparison.valueDifference(a, b) == .unknown)
    a.contentHash = "digest"
    #expect(RunComparison.valueDifference(a, b) == .unknown)
    b.contentHash = "digest"
    #expect(RunComparison.valueDifference(a, b) == .same)
    b.contentHash = "other"
    #expect(RunComparison.valueDifference(a, b) == .changed)
    b.contentHash = "digest"
    b.rootType = "Other"
    #expect(RunComparison.valueDifference(a, b) == .changed)
  }

}
