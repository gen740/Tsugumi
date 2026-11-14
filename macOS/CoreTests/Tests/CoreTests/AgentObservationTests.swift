import Foundation
import Testing
import TsugumiRPC
@testable import TsugumiAgentClient

struct AgentObservationTests {
  private struct Transport: ToolTransport {
    func project() async throws -> AgentProject? { nil }
  }

  private actor RestartingService {
    var calls: [String] = []
    var cursors: [UInt64] = []
    var polls = 0
    var snapshots = 0
    func call(_ method: String, _ fields: [String: Any]) throws -> [String: Any] {
      calls.append(method)
      let conversation = Tsugumi_Agent_ConversationT()
      conversation.id = "observed"
      if method == "ConversationCreateRequest" || method == "ConversationGetRequest" {
        // Even after the event connection has reconnected, the first snapshot
        // read can lose the race with the other connection being closed.
        if polls >= 3 {
          snapshots += 1
          if snapshots == 1 { throw RPCError.disconnected }
        }
        return ["conversation": conversation, "busy": polls < 3,
                "resumable": polls >= 3, "event_seq": UInt64(41)]
      }
      if method == "AccessListRequest" { return ["requests": [[String: Any]]()] }
      guard method == "EventsRequest" else { throw RPCError.unexpectedReply(method) }
      cursors.append(fields["after_seq"] as? UInt64 ?? 0)
      polls += 1
      if polls < 3 { throw RPCError.disconnected }
      // Restart marks the turn interrupted without appending a terminal event.
      return ["events": [[String: Any]](), "next_seq": UInt64(41), "busy": false]
    }
  }

  @Test func observationSurvivesFailedReconnectsAndReadsTheInterruptedSnapshot() async throws {
    let service = RestartingService()
    let agent = Agent(id: "observed", transport: Transport(), provider: .fixture,
                      request: { try await service.call($0, $1) })
    let events = AsyncStream<AgentEvent>.makeStream()
    await agent.observe { events.continuation.yield($0) }
    defer { events.continuation.finish() }
    var interrupted = false
    for await event in events.stream {
      if case .interrupted = event { interrupted = true; break }
      if case .error(let message) = event { Issue.record("Observation failed: \(message)"); break }
    }
    await agent.detach()
    #expect(interrupted)
    #expect(await agent.resumable)
    #expect(await agent.hasUnfinishedWork == false)
    #expect(await service.cursors == [41, 41, 41])
    #expect(await service.snapshots == 2)
    #expect(await service.calls.allSatisfy { ["ConversationCreateRequest", "ConversationGetRequest", "AccessListRequest", "EventsRequest"].contains($0) })
  }

  @Test func observationDoesNotRetryProtocolErrors() async throws {
    var attempts = 0
    do {
      let _: Int = try await AgentObservation.read(pause: { Issue.record("Must not retry a protocol error") }) {
        attempts += 1
        throw RPCError.protocolViolation("invalid frame")
      }
      Issue.record("Expected protocol failure")
    } catch RPCError.protocolViolation { }
    #expect(attempts == 1)
  }
}
