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
    XCTAssertFalse(ServiceWire.hostMethod("ConfigureRequest", [:]))
    XCTAssertFalse(ServiceWire.hostMethod("ConfigureRequest", ["conversation_id": "c"]))
  }

  func testExecutionSettingsUseTheHostContract() throws {
    for (method, fields) in [
      ("ExecutionServicesRequest", [:]),
      ("ExecutionServicePutRequest", ["id": "compute", "name": "Compute", "endpoint": "unix:/tmp/worker", "workerPublicKey": "ed25519:test"]),
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

  func testSettingsAndSignInUseTheHostContract() throws {
    for (method, fields) in [
      ("SettingsGetRequest", ["profile": "desktop"]),
      ("SettingsUpdateRequest", ["profile": "desktop", "revision": UInt64.max - 1, "changes": [["key": "agent.webSearch", "value_json": "true"]]]),
      ("ProviderSetRequest", ["profile": "desktop", "id": "openai", "remove": true]),
      ("ProviderStatusRequest", ["profile": "desktop", "id": "openai"]),
      ("SignInBeginRequest", ["profile": "desktop"]),
      ("SignInActionRequest", ["profile": "desktop", "attempt_id": "attempt", "action": "cancel"]),
      ("SignInStatusRequest", ["profile": "desktop", "attempt_id": "attempt"]),
    ] as [(String, [String: Any])] {
      XCTAssertTrue(ServiceWire.hostMethod(method, fields))
      let bytes = try ServiceWire.encode(method, fields: fields, id: 2, host: true)
      var buffer = ByteBuffer(data: bytes)
      let request: Tsugumi_Host_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSH1")
      XCTAssertEqual(request.requestId, 2)
      XCTAssertThrowsError(try ServiceWire.encode(method, fields: fields, id: 2, host: false))
    }
    for method in ["CredentialTokenRequest", "CredentialImportRequest", "CredentialForgetRequest", "ConfigureRequest"] {
      XCTAssertThrowsError(try ServiceWire.encode(method, fields: [:], id: 2, host: true))
    }
  }

  func testOmittedHostListVectorsAreEmpty() throws {
    for type in [Tsugumi_Host_ReplyPayload.recentlist, .demolist, .modellist, .executionservices] {
      var b = FlatBufferBuilder(initialSize: 128)
      let value: Offset
      switch type {
      case .recentlist: value = Tsugumi_Host_RecentList.createRecentList(&b)
      case .demolist: value = Tsugumi_Host_DemoList.createDemoList(&b)
      case .modellist: value = Tsugumi_Host_ModelList.createModelList(&b)
      default:
        let key = b.create(string: "ed25519:fixture")
        value = Tsugumi_Host_ExecutionServices.createExecutionServices(&b, hostPublicKeyOffset: key)
      }
      let reply = Tsugumi_Host_Reply.createReply(&b, valueType: type, valueOffset: value)
      let message = Tsugumi_Host_Message.createMessage(&b, requestId: 1, payloadType: .reply, payloadOffset: reply)
      Tsugumi_Host_Message.finish(&b, end: message)
      let decoded = try ServiceWire.decode(b.data, host: true)
      let list = try XCTUnwrap(decoded[type == .executionservices ? "services" : "value"] as? [Any])
      XCTAssertTrue(list.isEmpty)
    }
  }

  func testOmittedSettingsAndAccountVectorsAreEmpty() throws {
    for type in [Tsugumi_Host_ReplyPayload.settings, .signinstatus] {
      var b = FlatBufferBuilder(initialSize: 128)
      let value: Offset
      if type == .settings { value = Tsugumi_Host_Settings.createSettings(&b) }
      else {
        let state = b.create(string: "idle")
        value = Tsugumi_Host_SignInStatus.createSignInStatus(&b, stateOffset: state)
      }
      let reply = Tsugumi_Host_Reply.createReply(&b, valueType: type, valueOffset: value)
      let message = Tsugumi_Host_Message.createMessage(&b, requestId: 1, payloadType: .reply, payloadOffset: reply)
      Tsugumi_Host_Message.finish(&b, end: message)
      let decoded = try ServiceWire.decode(b.data, host: true)
      let result = try XCTUnwrap(decoded["value"] as? [String: Any])
      XCTAssertTrue(try XCTUnwrap(result[type == .settings ? "values" : "accounts"] as? [Any]).isEmpty)
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
