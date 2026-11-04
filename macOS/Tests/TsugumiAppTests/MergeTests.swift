import Foundation
import Testing
import TsugumiRPC
@testable import TsugumiApp

@Test func mergePreservesNamedRecordsAndOrderedValues() throws {
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../..")
  let data = try Data(contentsOf: root.appendingPathComponent("tests/fixtures/merge-contract.json"))
  let cases = try #require(try JSONSerialization.jsonObject(with: data) as? [[String: Any]])
  for c in cases {
    func value(_ key: String) throws -> JSONValue {
      let data = try JSONSerialization.data(withJSONObject: c[key]!)
      return try JSONValue.parse(String(decoding: data, as: UTF8.self))
    }
    let result = try #require(merge3(base: try value("base"), ours: try value("ours"), theirs: try value("theirs")))
    let actual = try JSONSerialization.jsonObject(with: Data(result.serialized().utf8)) as! NSObject
    #expect(actual.isEqual(c["expect"]!), "\(c["name"]!)")
  }
}
