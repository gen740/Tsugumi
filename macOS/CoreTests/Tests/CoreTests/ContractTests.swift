import FlatBuffers
import Foundation
import Testing
import TsugumiAgent
import TsugumiRPC

/// The Swift Agent's half of the Swift/web contract (tests/fixtures/; the web
/// half is web/scripts/contract.mjs): the tool catalog equals the snapshot,
/// and FlatJSON and the Draft request give the shared cases' results.
struct ContractTests {
  static let fixtures = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("../../../../tests/fixtures").standardizedFileURL
  let schema: BFBS
  let codec: FlatJSON
  let fixture: [String: Any]

  init() throws {
    schema = try BFBS(data: try #require(RPCSchema.bfbs()))
    codec = FlatJSON(schema: schema)
    let data = try Data(contentsOf: Self.fixtures.appendingPathComponent("agent-contract.json"))
    fixture = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
  }

  /// JSON values compared as JSON (numbers by value, objects without order).
  func same(_ a: Any?, _ b: Any?) throws -> Bool {
    func normal(_ v: Any?) throws -> Any {
      try JSONSerialization.jsonObject(with: JSONSerialization.data(withJSONObject: [v ?? NSNull()]))
    }
    return (try normal(a) as! NSArray).isEqual(try normal(b) as! NSArray)
  }

  @Test func toolCatalogEqualsTheSnapshot() throws {
    let tools = try ToolCatalog(schema: schema).tools.map {
      ["name": $0.name, "payloadType": Int($0.payloadType), "description": $0.description, "inputSchema": $0.inputSchema,
       "mutating": $0.mutating] as [String: Any]
    }
    let snapshot = try JSONSerialization.jsonObject(with: Data(contentsOf: Self.fixtures.appendingPathComponent("agent-tools.json")))
    #expect(try same(tools, snapshot), "the tool catalog differs from tests/fixtures/agent-tools.json")
  }

  @Test func flatJSONCases() throws {
    for c in try #require(fixture["flatjson"] as? [[String: Any]]) {
      let type = try #require(c["type"] as? String), json = try #require(c["json"] as? [String: Any])
      let object = try #require(schema.object(named: type)).0
      // A fresh builder each time (one a failed build used is left mid-object).
      var b = FlatBufferBuilder()
      if c["error"] as? Bool == true {
        #expect(throws: (any Error).self, "\(type) \(json)") { _ = try codec.build(object, json, into: &b) }
        continue
      }
      b.finish(offset: try codec.build(object, json, into: &b))
      let bb = b.sizedBuffer
      let out = codec.json(object, Table(bb: bb, position: Int32(bb.read(def: UInt32.self, position: bb.reader)) + Int32(bb.reader)))
      for (k, v) in c["expect"] as? [String: Any] ?? [:] {
        #expect(try same(out[k], v), "\(type) \(json): \(k) is \(String(describing: out[k]))")
      }
    }
  }

  @Test func approvalRequests() throws {
    for c in try #require(fixture["approvalRequired"] as? [[String: Any]]) {
      #expect(Agent.approvalRequired(try #require(c["json"] as? String)) == c["expect"] as? [String], "\(c)")
    }
  }

  @Test func draftRequests() throws {
    for c in try #require(fixture["draftRequest"] as? [[String: Any]]) {
      let cards = try #require(c["cards"] as? [[String: String]]).map {
        (key: $0["key"]!, definition: $0["definition"]!, description: $0["description"]!, content: $0["content"]!)
      }
      #expect(Agent.draftRequest(scope: try #require(c["scope"] as? [String]), cards: cards) == c["expect"] as? String)
    }
  }
}
