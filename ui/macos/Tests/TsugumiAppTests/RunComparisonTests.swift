import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiRPC

@MainActor struct RunComparisonTests {
  @Test func equalNamesDoNotMergeDifferentRunTargets() {
    let left = Tsugumi_RPC_RunSummaryT(), right = Tsugumi_RPC_RunSummaryT()
    left.target = cardPath(["aggregate-7k2q"])
    right.target = cardPath(["aggregate-9m3r"])
    left.propagate = true
    right.propagate = true
    let named = RunComparison.scope(left, right, displayPath: { _ in "Aggregate" })
    #expect(named.left == named.right && named.difference == .changed)
    right.target = left.target
    right.propagate = nil
    #expect(RunComparison.scope(left, right).difference == .same)
  }

  @Test func parametersAlignByNameAndRetainMissingSides() throws {
    func record(_ values: [(String, String)]) -> ComparedRunCard {
      let result = Tsugumi_RPC_RecordResultT()
      result.record = Tsugumi_RPC_InvocationRecordT()
      let params = Tsugumi_Data_InvocationParamsT()
      params.entries = values.map { name, value in
        let param = Tsugumi_Data_InvocationParamT()
        param.name = name
        param.value = value
        return param
      }
      result.record?.params = params
      return ComparedRunCard(summary: Tsugumi_RPC_InvocationSummaryT(), record: result)
    }
    let a = record([("low", "600"), ("boundary", "1650")])
    let b = record([("boundary", "1800"), ("low", "600"), ("added", "true")])
    let fields = RunComparison.params(a, b)
    let added = try #require(fields.first { $0.name == "added" })
    let boundary = try #require(fields.first { $0.name == "boundary" })
    let low = try #require(fields.first { $0.name == "low" })
    #expect(added.changed && added.left == nil && added.right == "true")
    #expect(boundary.changed && boundary.left == "1650" && boundary.right == "1800")
    #expect(!low.changed && low.left == "600" && low.right == "600")
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

@MainActor @Test func artifactIdsParticipateInValueComparison() {
  func value(_ id: String) -> Tsugumi_Data_NamedValueT {
    let v = Tsugumi_Data_NamedValueT()
    v.name = "value"
    v.contentHash = "bytes"
    let artifact = Tsugumi_Data_ValueArtifactT()
    artifact.id = id
    artifact.sizeBytes = 1
    v.artifacts = [artifact]
    return v
  }
  let a = value("id"), b = value("id")
  #expect(RunComparison.valueDifference(a, b) == .same)
  b.artifacts = value("other").artifacts
  #expect(RunComparison.valueDifference(a, b) == .changed)
  b.artifacts = a.artifacts
  b.rootType = "Other"
  #expect(RunComparison.valueDifference(a, b) == .changed)
  #expect(RunComparison.valueDifference(a, nil) == .unknown)
  #expect(RunComparison.valueDifference(nil, a) == .unknown)
  #expect(RunComparison.valueDifference(nil, nil) == .same)
  for output in [false, true] {
    let x = Tsugumi_RPC_RecordResultT(), y = Tsugumi_RPC_RecordResultT()
    x.record = Tsugumi_RPC_InvocationRecordT(); y.record = Tsugumi_RPC_InvocationRecordT()
    x.record?.recordIo = true; y.record?.recordIo = true
    if output { x.record?.outputs = [a] } else { x.record?.inputs = [a] }
    let left = ComparedRunCard(summary: Tsugumi_RPC_InvocationSummaryT(), record: x)
    let right = ComparedRunCard(summary: Tsugumi_RPC_InvocationSummaryT(), record: y)
    #expect(RunComparison.values(output, left, right).first?.difference == .unknown)
    #expect(RunComparison.values(output, nil, left).first?.difference == .unknown)
  }
}
