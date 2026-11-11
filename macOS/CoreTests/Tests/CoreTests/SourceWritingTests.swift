import Foundation
import Testing
@testable import TsugumiAgentClient

/// The arguments of a save, cut at every place they could be cut.
struct SourceWritingTests {
  private func scan(_ pieces: [String]) -> SourceWritingScanner {
    var s = SourceWritingScanner()
    for p in pieces { s.feed(p) }
    return s
  }

  private let save = #"""
  {"base_rev": "3", "changes": [
    {"value_type": "PutDefinition", "value": {"id": "load", "card_json": "{\"path\": \"no\", \"bytes\": \"no\"}", "keep_meta": true}},
    {"value_type": "PutSource", "value": {"path": "definitions/load/src/main.py", "bytes": "def main(ctx):\n\tprint(\"日本語 é 😀\")\n"}}
  ]}
  """#

  @Test func aFileIsReadWhereverTheStreamIsCut() {
    let whole = scan([save])
    #expect(whole.current?.path == "definitions/load/src/main.py")
    #expect(whole.current?.text == "def main(ctx):\n\tprint(\"日本語 é 😀\")\n")
    #expect(whole.current?.done == true)
    for at in 1..<save.count {
      let cut = save.index(save.startIndex, offsetBy: at)
      #expect(scan([String(save[..<cut]), String(save[cut...])]).current == whole.current)
    }
    #expect(scan(save.map { String($0) }).current == whole.current)
  }

  @Test func onlyWhatHasArrivedIsThere() {
    var s = SourceWritingScanner()
    s.feed(#"{"changes": [{"value": {"path": "definitions/a/x.py", "bytes": "imp"#)
    #expect(s.current == SourceWriting(path: "definitions/a/x.py", text: "imp", done: false, sequence: 1, ready: true))
    s.feed(#"ort os"#)
    #expect(s.current?.text == "import os" && s.current?.done == false)
    s.feed(#"", "base_hash": "h"}"#)
    #expect(s.current?.done == true)
    // The next file replaces it from its first key.
    s.feed(#"}, {"value": {"bytes": "b", "path": "definitions/b/y.py"#)
    #expect(s.current == SourceWriting(path: nil, text: "b", done: false, sequence: 2, ready: true))
    s.feed(#""}}]}"#)
    #expect(s.current == SourceWriting(path: "definitions/b/y.py", text: "b", done: true, sequence: 2, ready: true))
  }

  @Test func otherChangesAreNotFiles() {
    #expect(scan([#"{"changes": [{"value_type": "RemoveSource", "value": {"note": "path"}}, {"value": {"id": "bytes", "patches": [{"text": "x"}]}}]}"#]).current?.path == nil)
  }
  @Test func patchesKeepTheirIdentityWhenThePathArrivesLast() {
    let value = #"{"changes":[{"value_type":"PatchSource","value":{"patches":[{"offset":0,"remove":2,"text":"first"},{"text":"日本語 \ud83d\ude00","offset":4,"remove":1}],"path":"definitions/a/main.py"}}]}"#
    let whole = scan([value])
    #expect(whole.current?.path == "definitions/a/main.py")
    #expect(whole.current?.patch == true && whole.current?.ready == true)
    #expect(whole.current?.text == "日本語 😀" && whole.current?.done == true)
    for at in 1..<value.count {
      let cut = value.index(value.startIndex, offsetBy: at)
      #expect(scan([String(value[..<cut]), String(value[cut...])]).current == whole.current)
    }
  }

  @Test func anEmptyReplacementDoesNotKeepThePreviousPatch() {
    var scanner = scan([#"{"value":{"path":"definitions/a/main.py","patches":[{"text":"first"},{"text":""#])
    #expect(scanner.current?.text == "" && scanner.current?.patch == true && scanner.current?.ready == true)
    scanner.feed(#""}]}}"#)
    #expect(scanner.current?.done == true)
  }

}
