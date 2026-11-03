import FlatBuffers
import Foundation
import Testing
import TsugumiAgent
import TsugumiRPC

/// JSON <-> FlatBuffers at the edges of the integer types: 64 bit values
/// beyond JSON's exact range travel as decimal strings, both ways.
struct FlatJSONTests {
  let schema: BFBS
  let codec: FlatJSON
  init() throws {
    schema = try BFBS(data: try #require(RPCSchema.bfbs()))
    codec = FlatJSON(schema: schema)
  }

  func object(_ name: String) throws -> Int { try #require(schema.object(named: name)).0 }

  /// Builds into a fresh builder (one a failed build used is left mid-object).
  func build(_ object: Int, _ json: [String: Any]) throws {
    var b = FlatBufferBuilder()
    _ = try codec.build(object, json, into: &b)
  }

  func roundTrip(_ name: String, _ json: [String: Any]) throws -> [String: Any] {
    var b = FlatBufferBuilder()
    let root = try codec.build(try object(name), json, into: &b)
    b.finish(offset: root)
    let bb = b.sizedBuffer
    return codec.json(try object(name), Table(bb: bb, position: Int32(bb.read(def: UInt32.self, position: bb.reader)) + Int32(bb.reader)))
  }

  @Test func int64ScalarsAtTheirBounds() throws {
    for v: Int64 in [.min, .min + 1, -9_007_199_254_740_992, -9_007_199_254_740_991, 0, 9_007_199_254_740_991, .max] {
      let out = try roundTrip("Tsugumi.RPC.RunStatus", ["run_id": "r", "accepted_at": String(v)])
      let exact = v >= -9_007_199_254_740_991 && v <= 9_007_199_254_740_991
      if exact { #expect(out["accepted_at"] as? Int64 == v, "\(v): \(out)") } else { #expect(out["accepted_at"] as? String == String(v), "\(v): \(out)") }
    }
    let max = try roundTrip("Tsugumi.RPC.RunStatus", ["run_id": "r", "rev": String(UInt64.max)])
    #expect(max["rev"] as? String == String(UInt64.max))
  }

  @Test func outOfRangeValuesAreRejected() throws {
    let status = try object("Tsugumi.RPC.RunStatus")
    #expect(throws: (any Error).self) { try build(status, ["run_id": "r", "accepted_at": "9223372036854775808"]) }
    #expect(throws: (any Error).self) { try build(status, ["run_id": "r", "rev": "-1"]) }
    #expect(throws: (any Error).self) { try build(status, ["run_id": "r", "accepted_at": 9_007_199_254_740_993.0]) }
  }

  @Test func arrayElementsAtTheirBounds() throws {
    let cloud = try object("Tsugumi.Data.DisplayPointCloud")
    let ok = try roundTrip("Tsugumi.Data.DisplayPointCloud", ["x": [0.5], "y": [-1.25], "z": [0], "colors": [0, 4_294_967_295]])
    #expect((ok["colors"] as? [Any])?.compactMap { ($0 as? NSNumber)?.uint64Value } == [0, 4_294_967_295], "\(ok)")
    #expect((ok["y"] as? [Double]) == [-1.25], "\(ok)")
    #expect(throws: (any Error).self) { try build(cloud, ["x": [], "y": [], "z": [], "colors": [4_294_967_296]]) }
    #expect(throws: (any Error).self) { try build(cloud, ["x": [], "y": [], "z": [], "colors": [-1]]) }
    #expect(throws: (any Error).self) { try build(cloud, ["x": ["one"], "y": [], "z": []]) }
  }
}
