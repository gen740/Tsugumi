import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiRPC

@Test func draftOutputsRoundTrip() {
  let ports = [DraftPort(name: "report", type: "file"), DraftPort(name: "total", type: "ulong"), DraftPort(name: "label", type: "string")]
  let fbs = DraftPort.fbs(ports, namespace: "draft_x")
  #expect(fbs.contains(#"report:ReportValue (tsugumi: "file");"#))
  #expect(fbs.contains("table TotalValue { value:ulong; }"))
  let back = DraftPort.parse(fbs)
  #expect(back.map(\.name) == ["report", "total", "label"])
  #expect(back.map(\.type) == ["file", "ulong", "string"])
  #expect(DraftPort.fbs([], namespace: "x").isEmpty)
}

@Test func fileTreeGroupsFolders() {
  let t = FileNode.tree(["default.nix", "src/main.py", "src/lib/a.py", "_outputs.fbs"], base: "definitions/d/")
  #expect(t.map(\.name) == ["src", "_outputs.fbs", "default.nix"])
  let src = t[0]
  #expect(src.children?.map(\.name) == ["lib", "main.py"])
  #expect(src.children?[1].path == "definitions/d/src/main.py")
}

@Test func paramsKeepExactIntegerSpelling() {
  #expect(parseParam("18446744073709551615", "uint64") == .number("18446744073709551615"))
  #expect(parseParam("-3", "int64") == .number("-3"))
  #expect(parseParam("1.5e3", "double") == .number("1.5e3"))
  // Values the core must reject are passed on as text, never coerced.
  #expect(parseParam("12abc", "uint64") == .string("12abc"))
  #expect(parseParam(" x ", "string") == .string(" x "))
}

@Test func skillsLoadFromFoldersAndFiles() throws {
  let dir = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-skills-\(UUID().uuidString)")
  try FileManager.default.createDirectory(at: dir.appendingPathComponent("review"), withIntermediateDirectories: true)
  try "---\nname: review\ndescription: Review a Card\n---\nCheck the outputs.".write(
    to: dir.appendingPathComponent("review/SKILL.md"), atomically: true, encoding: .utf8)
  try "# Explain runs\nSay what happened.".write(to: dir.appendingPathComponent("explain.md"), atomically: true, encoding: .utf8)
  try "ignored".write(to: dir.appendingPathComponent("notes.txt"), atomically: true, encoding: .utf8)
  defer { try? FileManager.default.removeItem(at: dir) }
  let skills = loadSkills(in: dir, source: "test")
  #expect(skills.map(\.name) == ["explain", "review"])
  #expect(skills[0].description == "Explain runs")
  #expect(skills[1].description == "Review a Card")
}

@Test func collapsedCardsAnchorPortsOnTheHeader() {
  let open = CardGeometry(inputs: ["a", "b"], outputs: ["x"], collapsed: false, summary: true, params: 1, result: true)
  let closed = CardGeometry(inputs: ["a", "b"], outputs: ["x"], collapsed: true, summary: true, params: 1, result: true)
  #expect(closed.height == CardGeometry.header)
  #expect(closed.port("b", output: false) == CGPoint(x: 0, y: CardGeometry.header / 2))
  #expect(open.port("b", output: false).y > open.port("a", output: false).y)
  #expect(open.port("x", output: true).x == CardGeometry.width)
}

@Test func shortcutsHaveNoDefaultConflicts() {
  let keys = ShortcutAction.allCases.map(\.defaultKey)
  #expect(Set(keys).count == keys.count)
}
