import FlatBuffers
import Foundation
import Testing
import TsugumiRPC
@testable import TsugumiAgent

struct RecoveryTests {
  @Test func onlyTerminalReceiptsCompleteOperations() throws {
    let schema = try BFBS(data: try #require(RPCSchema.bfbs()))
    let codec = FlatJSON(schema: schema)
    let object = try #require(schema.object(named: "Tsugumi.RPC.OperationResult")).0
    for state in ["pending", "unknown", "success", "failure"] {
      let completed = ["success", "failure"].contains(state)
      var value: [String: Any] = ["state": completed ? "completed" : state]
      if completed {
        value["receipt"] = ["op_id": "op", "result_type": state == "failure" ? "Failure" : "SaveResult",
                            "result": state == "failure" ? ["kind": "validation_failed", "args": []] : [:]]
      }
      var b = FlatBufferBuilder()
      b.finish(offset: try codec.build(object, value, into: &b))
      let bb = b.sizedBuffer
      let result = codec.json(object, Table(bb: bb, position: Int32(bb.read(def: UInt32.self, position: bb.reader)) + Int32(bb.reader)))
      let data = try JSONSerialization.data(withJSONObject: ["reply": "OperationResult", "value": result])
      #expect(Agent.recoveredOperationError(String(decoding: data, as: UTF8.self)) == (completed ? state == "failure" : nil))
    }
    #expect(Agent.recoveredOperationError("not JSON") == nil)
  }
}
