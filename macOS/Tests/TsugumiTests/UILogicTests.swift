import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiRPC

@Test func stagesOfCodeAndBuiltinCards() {
  let code = StageProgress(stages: [.waiting, .prepare, .evaluate, .build, .execute, .validate, .save, .done], current: .build,
                           reached: [.prepare, .evaluate])
  #expect(code.stages.first == .waiting && code.stages.last == .done)
  #expect(Stage(phase: .evaluate) == .evaluate && Stage(phase: .save) == .save && Stage(phase: .unspecified) == nil)
  // Stages run in the order the core reports them.
  #expect(Stage.allCases.map(\.rawValue) == Array(0..<Stage.allCases.count))
}

@Test func wirePortsFollowTheFlow() {
  let data = CardNodeData(title: "x", kind: .code, subtitle: "", summary: nil,
                          inputs: [PortData(name: "a"), PortData(name: "b")], outputs: [PortData(name: "out")])
  let down = CardGeometry(data, flow: .topToBottom)
  #expect(down.port("a", output: false).y == 0 && down.port("out", output: true).y == down.height)
  #expect(down.port("a", output: false).x < down.port("b", output: false).x)
  let right = CardGeometry(data, flow: .leftToRight)
  #expect(right.port("a", output: false).x == 0 && right.port("out", output: true).x == CardGeometry.width)
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

@Test func collapsedCardsKeepTheirPortsOnTheEdges() {
  var data = CardNodeData(title: "x", kind: .code, subtitle: "", summary: "s",
                          inputs: [PortData(name: "a"), PortData(name: "b")], outputs: [PortData(name: "x")])
  data.collapsed = true
  let closed = CardGeometry(data, flow: .leftToRight)
  #expect(closed.height == CardGeometry.header)
  #expect(closed.port("b", output: false) == CGPoint(x: 0, y: CardGeometry.header / 2))
  let down = CardGeometry(data, flow: .topToBottom)
  #expect(down.port("x", output: true).y == CardGeometry.header)
}

@Test func shortcutsHaveNoDefaultConflicts() {
  let keys = ShortcutAction.allCases.map(\.defaultKey)
  #expect(Set(keys).count == keys.count)
}
