import FlatBuffers
import Foundation
import Testing
import TsugumiRPC
@testable import TsugumiAgent

private struct RecoveryTransport: ToolTransport, @unchecked Sendable {
  let schema: BFBS
  let reply: String
  let value: [String: Any]
  func call(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset) async throws -> (replyType: String, table: Table) {
    let codec = FlatJSON(schema: schema)
    var request = FlatBufferBuilder()
    request.finish(offset: try build(&request))
    let rb = request.sizedBuffer
    let args = codec.json(try #require(schema.object(named: "Tsugumi.RPC.OperationGetRequest")).0,
                          Table(bb: rb, position: Int32(rb.read(def: UInt32.self, position: rb.reader)) + Int32(rb.reader)))
    #expect(args["op_id"] as? String == "op")
    let name = reply == "Failure" ? "Tsugumi.Data.Failure" : "Tsugumi.RPC.\(reply)"
    var b = FlatBufferBuilder()
    b.finish(offset: try codec.build(try #require(schema.object(named: name)).0, value, into: &b))
    let bb = b.sizedBuffer
    return (reply, Table(bb: bb, position: Int32(bb.read(def: UInt32.self, position: bb.reader)) + Int32(bb.reader)))
  }
}

struct RecoveryTests {
  @Test func onlyTerminalReceiptsCompleteOperations() async throws {
    let schema = try BFBS(data: try #require(RPCSchema.bfbs()))
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: dir) }
    for c in try agentBehaviorCases("recovery") {
      let name = try #require(c["name"] as? String)
      let expected = c["error"] as? Bool
      let reply = try #require(c["reply"] as? String)
      let value = try #require(c["value"] as? [String: Any])
      let storage = dir.appendingPathComponent(name + ".json")
      var conversation = Conversation()
      conversation.tools = [ToolRecord(id: "t", name: "project_save", arguments: "{}", opId: "op", state: .sent, isError: true)]
      conversation.messages = [ChatMessage(role: "user", content: [.toolResult(toolUseId: "t", content: "old", isError: true)])]
      try conversation.save(storage)
      let agent = try Agent(catalog: try ToolCatalog(schema: schema), transport: RecoveryTransport(schema: schema, reply: reply, value: value),
                            provider: .localFixture(port: 1), storage: storage)
      try await agent.reconcilePending { _ in }
      let saved = try Conversation.read(storage)
      #expect(saved.tools[0].state == (expected == nil ? .sent : .completed), "\(name)")
      if let expected { #expect(saved.tools[0].isError == expected, "\(name)") }
      guard case .toolResult(_, let result, let isError, _) = saved.messages[0].content[0] else {
        Issue.record("missing tool result: \(name)"); continue
      }
      #expect(result != "old" && isError == (expected ?? true), "\(name)")
      #expect(Agent.recoveredOperationError(result) == expected, "\(name)")
    }
    #expect(Agent.recoveredOperationError("not JSON") == nil)
  }
}
