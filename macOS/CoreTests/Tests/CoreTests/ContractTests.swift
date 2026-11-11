import FlatBuffers
import Foundation
import Testing
import TsugumiAgent
import TsugumiRPC

/// Swift's platform codec. Core behavior is exercised through public RPC in
/// tests/core; this only checks values crossing the UI's FlatBuffers adapter.
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

}
