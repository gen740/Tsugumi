import FlatBuffers
import Foundation
import Testing
import TsugumiRPC
@testable import TsugumiAgent

final class ApprovalTransport: ToolTransport, @unchecked Sendable {
  let schema: BFBS
  let storage: URL
  var calls = 0
  var firstOp: String?
  init(schema: BFBS, storage: URL) { self.schema = schema; self.storage = storage }
  func call(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset) async throws -> (replyType: String, table: Table) {
    let codec = FlatJSON(schema: schema)
    var request = FlatBufferBuilder()
    request.finish(offset: try build(&request))
    let bb = request.sizedBuffer
    let object = try #require(schema.object(named: "Tsugumi.RPC.ProjectSaveRequest")).0
    let args = codec.json(object, Table(bb: bb, position: Int32(bb.read(def: UInt32.self, position: bb.reader)) + Int32(bb.reader)))
    let op = try #require(args["op_id"] as? String)
    let saved = try #require(try Conversation.read(storage).tools.first)
    #expect(saved.opId == op && saved.state == .sent)
    calls += 1
    if calls == 1 { firstOp = op } else { #expect(op != firstOp) }
    let type = calls == 1 ? "Failure" : "SaveResult"
    let name = calls == 1 ? "Tsugumi.Data.Failure" : "Tsugumi.RPC.SaveResult"
    let value: [String: Any] = calls == 1
      ? ["kind": "approval_required", "args": [["key": "permission", "value": "network card"]]] : [:]
    var reply = FlatBufferBuilder()
    reply.finish(offset: try codec.build(try #require(schema.object(named: name)).0, value, into: &reply))
    let out = reply.sizedBuffer
    return (type, Table(bb: out, position: Int32(out.read(def: UInt32.self, position: out.reader)) + Int32(out.reader)))
  }
}

/// Counts the RPCs sent and answers each with an empty file (FileContent).
final class CountingTransport: ToolTransport, @unchecked Sendable {
  let schema: BFBS
  var calls = 0
  init(schema: BFBS) { self.schema = schema }
  func call(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset) async throws -> (replyType: String, table: Table) {
    calls += 1
    let codec = FlatJSON(schema: schema)
    var reply = FlatBufferBuilder()
    reply.finish(offset: try codec.build(try #require(schema.object(named: "Tsugumi.RPC.FileContent")).0,
                                     ["name": "", "hash": "", "bytes": ""], into: &reply))
    let out = reply.sizedBuffer
    return ("FileContent", Table(bb: out, position: Int32(out.read(def: UInt32.self, position: out.reader)) + Int32(out.reader)))
  }
}

struct AgentOperationTests {
  /// A folder detached from the chat stays shared with the environment until it
  /// next starts: the Agent refuses it at once, and never sends the read.
  @Test func onlyAttachedFoldersAreRead() async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: dir) }
    let schema = try BFBS(data: try #require(RPCSchema.bfbs()))
    let transport = CountingTransport(schema: schema)
    let agent = try Agent(catalog: try ToolCatalog(schema: schema), transport: transport,
                          provider: .localFixture(port: 1), storage: dir.appendingPathComponent("chat.json"))
    func read(_ folder: String) async throws -> (String, Bool) {
      let block = try await agent.runTool(id: UUID().uuidString, name: "attached_folder_read",
                                          input: "{\"folder\":\"\(folder)\",\"path\":\"\"}") { _ in }
      guard case .toolResult(_, let content, let isError, _) = block else { throw CancellationError() }
      return (content, isError)
    }
    var (text, error) = try await read("/Users/me/data")
    #expect(error && text.contains("not attached here") && transport.calls == 0)
    await agent.setFolders(["/Users/me/data/"])
    (_, error) = try await read("/Users/me/data")
    #expect(!error && transport.calls == 1)
    await agent.setFolders([])
    (text, error) = try await read("/Users/me/data")
    #expect(error && text.contains("not attached here") && transport.calls == 1)
  }

  @Test func approvedOperationIsSavedBeforeResending() async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: dir) }
    let schema = try BFBS(data: try #require(RPCSchema.bfbs()))
    let storage = dir.appendingPathComponent("chat.json")
    let transport = ApprovalTransport(schema: schema, storage: storage)
    let agent = try Agent(catalog: try ToolCatalog(schema: schema), transport: transport,
                      provider: .localFixture(port: 1), storage: storage)
    await agent.setApprover { Set($0) }
    _ = try await agent.runTool(id: "tool-1", name: "project_save", input: "{\"base_rev\":\"0\",\"changes\":[]}") { _ in }
    #expect(transport.calls == 2)
  }
}
