import Foundation
import Testing
import TsugumiRPC

@MainActor
struct DocumentRevisionsTests {
  private func delta(_ base: UInt64, _ operation: String) -> Tsugumi_RPC_DocumentDeltaT {
    let value = Tsugumi_RPC_DocumentDeltaT()
    value.baseRev = base; value.rev = base + 1; value.opId = operation
    return value
  }

  @Test func replyAndEchoUseOneOrderedReducer() async throws {
    for echoFirst in [false, true] {
      let stream = DocumentRevisions()
      stream.reset(10)
      var seen: [UInt64] = []
      let apply: (Tsugumi_RPC_DocumentDeltaT, Bool) -> Void = { value, _ in seen.append(value.rev!) }
      let own = delta(11, "mine")
      if echoFirst { stream.receive(own, apply: apply) }
      stream.receive(own, apply: apply) // The operation reply.
      #expect(seen.isEmpty)
      stream.receive(delta(10, "other"), apply: apply)
      try await stream.applied(12)
      if !echoFirst { stream.receive(own, apply: apply) }
      stream.receive(own, apply: apply)
      #expect(seen == [11, 12])
      stream.receive(delta(12, "second-operation"), apply: apply)
      try await stream.applied(13)
      #expect(seen == [11, 12, 13])
    }
  }

  @Test func snapshotResolvesAlreadyAppliedReplies() async throws {
    let stream = DocumentRevisions()
    stream.reset(20)
    try await stream.applied(19)
    var calls = 0
    stream.receive(delta(19, "old-reply")) { _, _ in calls += 1 }
    #expect(calls == 0)
  }
}

extension DocumentRevisionsTests {
  @Test func aReplyWaitsForTheEarlierNotificationToBeApplied() async throws {
    let stream = DocumentRevisions()
    var seen: [UInt64] = []
    var completed = false
    let apply: (Tsugumi_RPC_DocumentDeltaT, Bool) -> Void = { value, _ in seen.append(value.rev!) }
    stream.receive(delta(1, "reply"), apply: apply)
    var waiter: Task<Void, Error>!
    // The main actor runs the waiter through its suspension before returning
    // this explicit transport barrier. No timer or scheduling poll is needed.
    await withCheckedContinuation { (entered: CheckedContinuation<Void, Never>) in
      waiter = Task { @MainActor in
        entered.resume()
        try await stream.applied(2)
        completed = true
      }
    }
    #expect(!completed)
    #expect(seen.isEmpty)
    stream.receive(delta(0, "notification"), apply: apply)
    try await waiter.value
    #expect(completed)
    #expect(seen == [1, 2])
  }
}
