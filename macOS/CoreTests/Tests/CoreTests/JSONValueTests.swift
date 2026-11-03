import Testing
@testable import TsugumiRPC

@Test func preservesIntegerSpellingAndOrder() throws {
  let text = #"{"b":18446744073709551615,"a":[1,2.50,true,null],"s":"日本\n"}"#
  let v = try JSONValue.parse(text)
  #expect(v["b"]?.numberText == "18446744073709551615")
  #expect(v.object?.map(\.0) == ["b", "a", "s"])
  #expect(v.serialized(pretty: false) == #"{"b":18446744073709551615,"a":[1,2.50,true,null],"s":"日本\n"}"#)
}

@Test func rejectsDuplicateKeysAndTrailingData() {
  #expect(throws: JSONValue.ParseError.self) { try JSONValue.parse(#"{"a":1,"a":2}"#) }
  #expect(throws: JSONValue.ParseError.self) { try JSONValue.parse(#"{} x"#) }
}
