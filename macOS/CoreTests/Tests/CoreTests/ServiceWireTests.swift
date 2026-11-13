import FlatBuffers
import Foundation
import TsugumiRPC
import XCTest
@testable import TsugumiAgentClient

final class ServiceWireTests: XCTestCase {
  func testContractsAndExactSequenceNumbers() throws {
    let high = UInt64.max - 1
    let bytes = try ServiceWire.encode("EventsRequest", fields: ["conversation_id": "日本語", "after_seq": high], id: high, host: false)
    var buffer = ByteBuffer(data: bytes)
    let root: Tsugumi_Agent_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSA1")
    let message = Tsugumi_Agent_MessageT(root)
    XCTAssertEqual(message.requestId, high)
    let request = try XCTUnwrap(message.payload?.value as? Tsugumi_Agent_RequestT)
    let events = try XCTUnwrap(request.value?.value as? Tsugumi_Agent_EventsRequestT)
    XCTAssertEqual(events.afterSeq, high)
    XCTAssertEqual(events.conversationId, "日本語")
    XCTAssertThrowsError(try ServiceWire.encode("RegisterProjectRequest", fields: [:], id: 1, host: false))
    let host = try ServiceWire.encode("HostRecentListRequest", fields: [:], id: 1, host: true)
    XCTAssertThrowsError(try ServiceWire.decode(host, host: false))
    XCTAssertTrue(ServiceWire.hostMethod("RegisterProjectRequest", [:]))
    XCTAssertTrue(ServiceWire.hostMethod("ConfigureRequest", [:]))
    XCTAssertFalse(ServiceWire.hostMethod("ConfigureRequest", ["conversation_id": "c"]))
  }

  func testExecutionSettingsUseTheHostContract() throws {
    for (method, fields) in [
      ("ExecutionServicesRequest", [:]),
      ("ExecutionServicePutRequest", ["id": "compute", "name": "Compute", "endpoint": "unix:/tmp/worker", "token": String(repeating: "a", count: 64)]),
      ("ExecutionServiceRemoveRequest", ["id": "compute"]),
      ("ExecutionProjectAuthorizeRequest", ["project_id": "project", "path": "/project", "control_dir": "/control"]),
      ("ExecutionProjectRevokeRequest", ["project_id": "project"]),
    ] as [(String, [String: Any])] {
      XCTAssertTrue(ServiceWire.hostMethod(method, fields))
      let bytes = try ServiceWire.encode(method, fields: fields, id: 2, host: true)
      var buffer = ByteBuffer(data: bytes)
      let request: Tsugumi_Host_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSH1")
      XCTAssertEqual(request.requestId, 2)
      XCTAssertThrowsError(try ServiceWire.encode(method, fields: fields, id: 2, host: false))
    }
  }

  func testTypedSnapshotBecomesPresentationValues() throws {
    let content = Tsugumi_Agent_ModelMessageT()
    content.role = "assistant"
    content.contentJson = #"[{"type":"text","text":"kept"}]"#
    let conversation = Tsugumi_Agent_ConversationT()
    conversation.id = "typed"
    conversation.updatedAt = 1_700_000_000_123
    conversation.messages = [content]
    let result = Tsugumi_Agent_ConversationResultT()
    result.conversation = conversation
    result.eventSeq = UInt64.max - 1
    let decoded = try ServiceWire.decode(reply(result, type: .conversationresult), host: false)
    let native = try XCTUnwrap(decoded["conversation"] as? Tsugumi_Agent_ConversationT)
    let snapshot = try ServiceWire.conversation(native)
    XCTAssertEqual(snapshot.id, "typed")
    XCTAssertEqual(snapshot.messages.first?.content, [.text("kept")])
    XCTAssertEqual(snapshot.updatedAt.timeIntervalSince1970, 1_700_000_000.123, accuracy: 0.001)
    XCTAssertEqual(decoded["event_seq"] as? UInt64, UInt64.max - 1)
  }

  func testFailuresKeepTheApprovalRequestIdentity() throws {
    let failure = Tsugumi_Service_FailureT()
    failure.code = "grant_required"
    failure.message = "Ask first"
    failure.requestId = "permission-1"
    XCTAssertThrowsError(try ServiceWire.decode(reply(failure, type: .failure), host: false)) { error in
      XCTAssertEqual((error as? CoreAgentFailure)?.code, "grant_required")
      XCTAssertEqual((error as? CoreAgentFailure)?.requestID, "permission-1")
    }
  }

  private func reply(_ value: NativeObject, type: Tsugumi_Agent_ReplyPayload) -> Data {
    let reply = Tsugumi_Agent_ReplyT()
    reply.value = Tsugumi_Agent_ReplyPayloadUnion(value, type: type)
    let message = Tsugumi_Agent_MessageT()
    message.requestId = 1
    message.payload = Tsugumi_Agent_PayloadUnion(reply, type: .reply)
    var object: Tsugumi_Agent_MessageT? = message
    var b = FlatBufferBuilder(initialSize: 1024)
    let offset = Tsugumi_Agent_Message.pack(&b, obj: &object)
    Tsugumi_Agent_Message.finish(&b, end: offset)
    return b.data
  }
}
