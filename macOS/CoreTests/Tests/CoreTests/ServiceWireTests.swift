import FlatBuffers
import Foundation
import TsugumiRPC
import XCTest
@testable import TsugumiAgentClient

final class ServiceWireTests: XCTestCase {
  func testSendCarriesTheOptimisticDisplaySeparatelyFromModelInput() throws {
    let bytes = try ServiceWire.encode("ConversationSendRequest", fields: [
      "conversation_id": "c", "turn_id": "t", "text": "Model instructions",
      "display": "hello instead", "selected_cards": [String](), "selected_displays": [String](),
    ], id: 1, host: false)
    var buffer = ByteBuffer(data: bytes)
    let message: Tsugumi_Agent_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSA1")
    let request = message.payload(type: Tsugumi_Agent_Request.self)!.value(type: Tsugumi_Agent_ConversationSendRequest.self)!
    XCTAssertEqual(request.text, "Model instructions")
    XCTAssertEqual(request.display, "hello instead")
  }

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

  func testGuideHostStateHasTopLevelPresentationAndEmptyVectors() throws {
    let bytes = try ServiceWire.encode("GuideAdvanceRequest", fields: ["session_id": "desktop", "language": "en",
      "event": ["kind": "message", "text": "Next", "expected_step": "report/start", "project": "/copy"]], id: 1, host: true)
    var buffer = ByteBuffer(data: bytes)
    let root: Tsugumi_Host_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSH1")
    let request = try XCTUnwrap(Tsugumi_Host_MessageT(root).payload?.value as? Tsugumi_Host_RequestT)
    let guide = try XCTUnwrap(request.value?.value as? Tsugumi_Host_GuideAdvanceRequestT)
    XCTAssertEqual(guide.event.expectedStep, "report/start")
    XCTAssertEqual(guide.event.scripted, true)
    XCTAssertTrue(ServiceWire.hostMethod("GuideGetRequest", [:]))
    var b = FlatBufferBuilder(initialSize: 128)
    let session = b.create(string: "desktop")
    let state = Tsugumi_Host_GuideState.createGuideState(&b, sessionIdOffset: session)
    let reply = Tsugumi_Host_Reply.createReply(&b, valueType: .guidestate, valueOffset: state)
    let message = Tsugumi_Host_Message.createMessage(&b, requestId: 1, payloadType: .reply, payloadOffset: reply)
    Tsugumi_Host_Message.finish(&b, end: message)
    let decoded = try ServiceWire.decode(b.data, host: true)
    XCTAssertNil(decoded["value"])
    XCTAssertEqual(decoded["sessionId"] as? String, "desktop")
    XCTAssertTrue(try XCTUnwrap(decoded["choices"] as? [String]).isEmpty)
    XCTAssertTrue(try XCTUnwrap(decoded["actions"] as? [Any]).isEmpty)
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
      XCTAssertTrue(try XCTUnwrap(decoded[type == .settings ? "values" : "accounts"] as? [Any]).isEmpty)
    }
  }

  func testEnvironmentRequestsAndTopLevelReplies() throws {
    let fields: [String: Any] = ["project": "/project", "action": Tsugumi_Host_EnvironmentAction.start, "archive_incompatible_history": true]
    XCTAssertTrue(ServiceWire.hostMethod("EnvironmentRequest", fields))
    let bytes = try ServiceWire.encode("EnvironmentRequest", fields: fields, id: 4, host: true)
    var buffer = ByteBuffer(data: bytes)
    let root: Tsugumi_Host_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSH1")
    let request = try XCTUnwrap((Tsugumi_Host_MessageT(root).payload?.value as? Tsugumi_Host_RequestT)?.value?.value as? Tsugumi_Host_EnvironmentRequestT)
    XCTAssertEqual(request.action, .start)
    XCTAssertTrue(request.archiveIncompatibleHistory)
    XCTAssertTrue(request.origins.isEmpty)
    XCTAssertTrue(request.folders.isEmpty)
    let state = Tsugumi_Host_EnvironmentStateT(); state.project = "/project"; state.key = "key"; state.phase = .ready; state.endpoint = "ws://localhost:1/rpc"
    let reply = Tsugumi_Host_ReplyT(); reply.value = Tsugumi_Host_ReplyPayloadUnion(state, type: .environmentstate)
    let message = Tsugumi_Host_MessageT(); message.requestId = 4; message.payload = Tsugumi_Host_PayloadUnion(reply, type: .reply)
    var builder = FlatBufferBuilder(initialSize: 128); var object: Tsugumi_Host_MessageT? = message
    let offset = Tsugumi_Host_Message.pack(&builder, obj: &object); Tsugumi_Host_Message.finish(&builder, end: offset)
    let decoded = try ServiceWire.decode(builder.data, host: true)
    XCTAssertNil(decoded["value"])
    XCTAssertEqual(decoded["phase"] as? Tsugumi_Host_EnvironmentPhase, .ready)
    XCTAssertEqual(decoded["endpoint"] as? String, state.endpoint)
  }

  func testRuntimeAndTerminalOmittedVectorsAndEvents() throws {
    let runtime = Tsugumi_Host_HostRuntimeT(); runtime.vmState = "stopped"
    runtime.virtualizationSupported = true; runtime.imageAvailable = true; runtime.imageVersion = "bundled-image"
    let reply = Tsugumi_Host_ReplyT(); reply.value = Tsugumi_Host_ReplyPayloadUnion(runtime, type: .hostruntime)
    let message = Tsugumi_Host_MessageT(); message.requestId = 1; message.payload = Tsugumi_Host_PayloadUnion(reply, type: .reply)
    var builder = FlatBufferBuilder(initialSize: 128); var object: Tsugumi_Host_MessageT? = message
    let offset = Tsugumi_Host_Message.pack(&builder, obj: &object); Tsugumi_Host_Message.finish(&builder, end: offset)
    let decoded = try ServiceWire.decode(builder.data, host: true)
    XCTAssertNil(decoded["value"])
    XCTAssertEqual(decoded["virtualization_supported"] as? Bool, true)
    XCTAssertEqual(decoded["image_available"] as? Bool, true)
    XCTAssertEqual(decoded["image_version"] as? String, "bundled-image")
    XCTAssertTrue(try XCTUnwrap(decoded["console"] as? [String]).isEmpty)
    XCTAssertTrue(try XCTUnwrap(decoded["histories"] as? [Any]).isEmpty)
    let terminal = Tsugumi_Host_HostTerminalDataT(); terminal.session = UInt64.max - 1; terminal.closed = true
    let event = Tsugumi_Host_EventT(); event.value = Tsugumi_Host_EventPayloadUnion(terminal, type: .hostterminaldata)
    message.requestId = 0; message.payload = Tsugumi_Host_PayloadUnion(event, type: .event)
    builder = FlatBufferBuilder(initialSize: 128); object = message
    let eventOffset = Tsugumi_Host_Message.pack(&builder, obj: &object); Tsugumi_Host_Message.finish(&builder, end: eventOffset)
    let notification = try ServiceWire.decodeEvent(builder.data)
    let value = try XCTUnwrap(notification["value"] as? [String: Any])
    XCTAssertEqual(value["session"] as? UInt64, UInt64.max - 1)
    XCTAssertTrue(try XCTUnwrap(value["data"] as? [UInt8]).isEmpty)
  }

  func testSettingsAndAuthenticationRepliesExposeTheirTypedFields() throws {
    let setting = Tsugumi_Host_SettingT()
    setting.key = "agent.model.openai"
    setting.valueJson = #""from-the-app""#
    let settings = Tsugumi_Host_SettingsT()
    settings.revision = UInt64.max - 1
    settings.values = [setting]
    let saved = try ServiceWire.decode(hostReply(settings, type: .settings), host: true)
    XCTAssertEqual(saved["revision"] as? UInt64, UInt64.max - 1)
    let values = try XCTUnwrap(saved["values"] as? [[String: String]])
    XCTAssertEqual(values, [["key": "agent.model.openai", "value_json": #""from-the-app""#]])

    let begin = Tsugumi_Host_SignInBeginT()
    begin.attemptId = "attempt-1"
    begin.url = "https://issuer.example/authorize"
    let opened = try ServiceWire.decode(hostReply(begin, type: .signinbegin), host: true)
    XCTAssertEqual(opened["attempt_id"] as? String, begin.attemptId)
    XCTAssertEqual(opened["url"] as? String, begin.url)

    let account = Tsugumi_Host_AccountT()
    account.clientId = "client-1"
    account.signedIn = true
    account.active = true
    let status = Tsugumi_Host_SignInStatusT()
    status.state = "complete"
    status.accounts = [account]
    status.revoked = true
    let polled = try ServiceWire.decode(hostReply(status, type: .signinstatus), host: true)
    XCTAssertEqual(polled["state"] as? String, "complete")
    XCTAssertEqual(polled["revoked"] as? Bool, true)
    let accounts = try XCTUnwrap(polled["accounts"] as? [[String: Any]])
    XCTAssertEqual(accounts.count, 1)
    XCTAssertEqual(accounts.first?["clientId"] as? String, "client-1")
    XCTAssertEqual(accounts.first?["signedIn"] as? Bool, true)

    let provider = Tsugumi_Host_ProviderStatusT()
    provider.id = "anthropic"
    provider.configured = true
    let provisioned = try ServiceWire.decode(hostReply(provider, type: .providerstatus), host: true)
    XCTAssertEqual(provisioned["id"] as? String, "anthropic")
    XCTAssertEqual(provisioned["configured"] as? Bool, true)
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

  func testInterruptionUsesTheCoreFlagsAndKeepsTheOperation() throws {
    let conversation = Tsugumi_Agent_ConversationT()
    conversation.id = "interrupted"
    let operation = Tsugumi_Agent_InterruptedOperationT()
    operation.kind = "tool"
    operation.id = "pending-read"
    operation.name = "project_inspect"
    operation.argumentsJson = #"{"at_rev":42}"#
    let result = Tsugumi_Agent_ConversationResultT()
    result.conversation = conversation
    result.resumable = true
    result.continuable = true
    result.interruptedOperation = operation
    let decoded = try ServiceWire.decode(reply(result, type: .conversationresult), host: false)
    XCTAssertEqual(decoded["busy"] as? Bool, false)
    XCTAssertEqual(decoded["resumable"] as? Bool, true)
    XCTAssertEqual(decoded["continuable"] as? Bool, true)
    let saved = try XCTUnwrap(decoded["interrupted_operation"] as? Tsugumi_Agent_InterruptedOperationT)
    XCTAssertEqual(saved.kind, "tool")
    XCTAssertEqual(saved.id, "pending-read")
    XCTAssertEqual(saved.name, "project_inspect")
    XCTAssertEqual(saved.argumentsJson, operation.argumentsJson)

    // An event page reports liveness only; it has no interruption flags.
    let page = Tsugumi_Agent_EventsPageT()
    let idle = try ServiceWire.decode(reply(page, type: .eventspage), host: false)
    XCTAssertEqual(idle["busy"] as? Bool, false)
    XCTAssertNil(idle["resumable"])
    XCTAssertNil(idle["interrupted_operation"])
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

  private func hostReply(_ value: NativeObject, type: Tsugumi_Host_ReplyPayload) -> Data {
    let reply = Tsugumi_Host_ReplyT()
    reply.value = Tsugumi_Host_ReplyPayloadUnion(value, type: type)
    let message = Tsugumi_Host_MessageT()
    message.requestId = 1
    message.payload = Tsugumi_Host_PayloadUnion(reply, type: .reply)
    var object: Tsugumi_Host_MessageT? = message
    var b = FlatBufferBuilder(initialSize: 1024)
    let offset = Tsugumi_Host_Message.pack(&b, obj: &object)
    Tsugumi_Host_Message.finish(&b, end: offset)
    return b.data
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
