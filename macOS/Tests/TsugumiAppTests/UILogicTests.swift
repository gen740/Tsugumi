import Foundation
import AppKit
import SwiftUI
import Testing
@testable import TsugumiApp
@testable import TsugumiRPC

@Test func stagesOfCodeAndBuiltinCards() {
  // Shown as build → run → validate (SDK and Nix inside build, saving inside validate).
  #expect(Stage.applicable(code: true) == [.build, .execute, .validate])
  #expect(Stage.applicable(code: false) == [.execute, .validate])
  #expect(Stage.build.parts == [.prepare, .evaluate, .build] && Stage.validate.parts == [.validate, .save])
  #expect(StageProgress.merge([.cached, .cached, .cached], ended: true) == .cached)
  #expect(StageProgress.merge([.done, .pending, .pending], ended: false) == .running)
  #expect(StageProgress.merge([.done, .failed, .notRun], ended: true) == .failed)
  #expect(StageProgress.merge([.reused, .reused, .reused], ended: true) == .reused)
  #expect(Stage(phase: .evaluate) == .evaluate && Stage(phase: .save) == .save && Stage(phase: .unspecified) == nil)
  // Stages run in the order the core reports them.
  #expect(Stage.allCases.map(\.rawValue) == Array(0..<Stage.allCases.count))
  // Every stage keeps its own icon, also while running (no generic check marks).
  #expect(Set(Stage.allCases.map(\.symbol)).count == Stage.allCases.count)
  #expect(Stage.build.activeSymbol == "hammer.fill" && Stage.evaluate.activeSymbol == "snowflake")
  #expect(Stage.build.logStream == .build && Stage.execute.logStream == .stdout && Stage.save.logStream == .journal)
}

@Test func runTextCountsFromTheFirstInvocation() {
  var d = CardNodeData(title: "x", kind: .code, summary: nil, inputs: [], outputs: [])
  #expect(d.runText == nil && d.idleText == "未実行（0 回）")
  d.state = .succeeded
  d.invocation = 1
  #expect(d.runText == "実行 #1 · 成功")
  d.runContinues = true
  #expect(d.runText!.hasPrefix("実行 #1 · 成功") && d.runText!.contains("次の入力"))
}

/// The bubble takes its ideal size: long text must wrap inside it, not run out of it.
@MainActor @Test func theHoverBubbleHoldsItsWrappedText() {
  func size(_ lines: [String]) -> CGSize {
    NSHostingView(rootView: HoverBubble(content: .init(title: "rgb", subtitle: "Draft", symbol: "circle", lines: lines, status: "未実装（Draft）",
                                                       problems: ["draft cards are not implemented and cannot run"]))
      .fixedSize()).fittingSize
  }
  let short = size(["短い説明"])
  let long = size([String(repeating: "3つの帯域画像を同じ座標と大きさのまま割り当てる。", count: 6)])
  #expect(long.width <= 361)
  #expect(long.height >= short.height + 40, "\(short) \(long)")
}

@Test func aDraftOffersItsNextPortAndOpensWhileWritten() {
  var data = CardNodeData(title: "x", kind: .draft, summary: "plan", inputs: [], outputs: [])
  let empty = CardGeometry(data, flow: .topToBottom)
  #expect(empty.newPortPoint(output: true) == CGPoint(x: CardGeometry.width / 2, y: empty.height))
  #expect(empty.port(CardGeometry.newPort, output: false) == CGPoint(x: CardGeometry.width / 2, y: 0))
  data.outputs = [PortData(name: "out")]
  let wired = CardGeometry(data, flow: .topToBottom)
  // The port is where the wire started; wires do not change the size of a Draft.
  #expect(wired.port("out", output: true) == CGPoint(x: CardGeometry.width / 2, y: wired.height))
  #expect(wired.newPortPoint(output: true) == nil, "a side with a port starts its wires from it")
  #expect(wired.height == empty.height)
  // Across a left-to-right flow: the middle of the side; several around it.
  let right = CardGeometry(data, flow: .leftToRight)
  #expect(right.port("out", output: true) == CGPoint(x: CardGeometry.width, y: right.height / 2))
  #expect(right.newPortPoint(output: false) == CGPoint(x: 0, y: right.height / 2))
  data.outputs = [PortData(name: "x"), PortData(name: "y")]
  let two = CardGeometry(data, flow: .leftToRight)
  #expect((two.port("x", output: true).y + two.port("y", output: true).y) / 2 == two.height / 2)
  // Without a name, a Draft is its content.
  data.untitled = true
  #expect(CardGeometry(data, flow: .topToBottom).headerRow == CardGeometry.untitledTop)
  data.untitled = false
  data.draftEditing = true
  #expect(CardGeometry(data, flow: .topToBottom).draftRow == CardGeometry.draftEditRow)
  data.draftEditing = false
  data.outputs = []
  data.collapsed = true
  #expect(CardGeometry(data, flow: .topToBottom).newPortPoint(output: true) == nil)
}

@Test func wirePortsFollowTheFlow() {
  let data = CardNodeData(title: "x", kind: .code, summary: nil,
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

@Test func theSmallCardsKeepTheirShapeWithASummary() {
  func geometry(_ kind: CardKind) -> CardGeometry {
    CardGeometry(CardNodeData(title: "x", kind: kind, summary: "what it does", inputs: [], outputs: []), flow: .topToBottom)
  }
  for kind in [CardKind.trigger, .fork, .merge, .branch] { #expect(!geometry(kind).summary, "\(kind)") }
  #expect(geometry(.code).summary)
  #expect(geometry(.subgraph).summary)
}

@Test func collapsedCardsKeepTheirPortsOnTheEdges() {
  var data = CardNodeData(title: "x", kind: .code, summary: "s",
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

@Test func routingCardsHaveTheirShapeAndPorts() {
  let branch = CardGeometry(CardNodeData(title: "b", kind: .branch, summary: nil,
                                         inputs: [PortData(name: "value"), PortData(name: "condition")],
                                         outputs: [PortData(name: "a"), PortData(name: "b")]), flow: .topToBottom)
  #expect(branch.shape == .branch)
  let v = branch.port("value", output: false), c = branch.port("condition", output: false)
  let a = branch.port("a", output: true), b = branch.port("b", output: true)
  // Data and condition never share a point; a (true) left, b (false) right, below the inputs.
  #expect(v != c && v.x < c.x && a.x < b.x && a.y > v.y && b.y > c.y)
  // Every port lies on the diamond's outline.
  let outline = branch.outline(in: branch.outlineRect)
  for p in [v, c, a, b] {
    #expect(outline.strokedPath(StrokeStyle(lineWidth: 2)).contains(p))
  }
  let fork = CardGeometry(CardNodeData(title: "f", kind: .fork, summary: nil, inputs: [PortData(name: "in")],
                                       outputs: [PortData(name: "o1"), PortData(name: "o2"), PortData(name: "o3")]), flow: .topToBottom)
  let top = fork.port("in", output: false)
  let outs = ["o1", "o2", "o3"].map { fork.port($0, output: true) }
  // Narrow top with one input, wide bottom edge with the outputs spread.
  #expect(abs(top.x - fork.size.width / 2) < 0.5 && top.y == fork.outlineRect.minY)
  #expect(outs.allSatisfy { $0.y == fork.outlineRect.maxY } && outs[0].x < outs[1].x && outs[1].x < outs[2].x)
  #expect(outs[2].x - outs[0].x > fork.size.width * 0.4)
  let merge = CardGeometry(CardNodeData(title: "m", kind: .merge, summary: nil,
                                        inputs: [PortData(name: "a"), PortData(name: "b")], outputs: [PortData(name: "out")]), flow: .topToBottom)
  #expect(abs(merge.port("out", output: true).x - merge.size.width / 2) < 0.5)
  #expect(merge.port("a", output: false).y == merge.outlineRect.minY)
}

@Test func portLabelsSitOnTheirPortsOrHide() {
  let few = CardGeometry(CardNodeData(title: "x", kind: .code, summary: nil, inputs: [PortData(name: "a"), PortData(name: "b")],
                                      outputs: []), flow: .topToBottom)
  #expect(few.showLabels(input: true))
  let many = CardGeometry(CardNodeData(title: "x", kind: .code, summary: nil,
                                       inputs: (0..<12).map { PortData(name: "port\($0)") }, outputs: []), flow: .topToBottom)
  // Twelve ports on 220 points leave no room: names are shown on hover instead.
  #expect(!many.showLabels(input: true) && many.topLabels == 0)
}

@Test func feedbackWiresGoAroundTheCards() {
  let cards = [CGRect(x: 0, y: 0, width: 220, height: 80), CGRect(x: 0, y: 160, width: 220, height: 80),
               CGRect(x: 0, y: 320, width: 220, height: 80)]
  var router = WireRouter(flow: .topToBottom, obstacles: cards)
  // From the bottom Card back up to the top one.
  let back = router.route(CGPoint(x: 110, y: 400), CGPoint(x: 110, y: 0))
  let middle = cards[1].insetBy(dx: 2, dy: 2)
  for t in stride(from: 0.02, through: 0.98, by: 0.02) {
    let p = back.trimmedPath(from: 0, to: t).currentPoint ?? .zero
    #expect(!middle.contains(p))
  }
  // A second feedback wire gets its own lane.
  let other = router.route(CGPoint(x: 100, y: 400), CGPoint(x: 100, y: 0))
  #expect(abs(other.boundingRect.minX - back.boundingRect.minX) >= 12)
  // Forward wires stay curves along the flow.
  #expect(router.isForward(CGPoint(x: 0, y: 80), CGPoint(x: 0, y: 160)))
}

@Test func editedOutputsAreReadFromTheText() {
  let ok = OutputCatalog.parse("""
    namespace t; // ports
    table Num { value:ulong; }
    table Outputs { out:Num (required); extra:Num; /* note */ }
    root_type Outputs;
    """)
  #expect(ok == .ports([PortData(name: "out", typeName: "Num", optional: false), PortData(name: "extra", typeName: "Num", optional: true)]))
  if case .error = OutputCatalog.parse("table Outputs { out:Num ") {} else { Issue.record("an unfinished catalog is an error") }
  if case .error = OutputCatalog.parse("table Outputs { out:Num; }") {} else { Issue.record("root_type is required") }
}

@Test func recentAgesHaveNoSeconds() {
  let now = Date()
  #expect(relativeAge(now.addingTimeInterval(-20), now: now) == "たった今")
  #expect(relativeAge(now.addingTimeInterval(-20 * 60 - 51), now: now) == "20 分前")
  #expect(relativeAge(now.addingTimeInterval(-3 * 3600), now: now) == "3 時間前")
  #expect(relativeAge(now.addingTimeInterval(-2 * 86400), now: now) == "2 日前")
}

@Test func cameraFitsInsideTheFreeArea() {
  var c = CanvasCamera()
  c.fit(CGRect(x: 0, y: 0, width: 400, height: 300), in: CGSize(width: 800, height: 600),
        insets: SwiftUI.EdgeInsets(top: 50, leading: 0, bottom: 50, trailing: 0), maxZoom: 1, margin: 0)
  let tl = c.screen(CGPoint(x: 0, y: 0)), br = c.screen(CGPoint(x: 400, y: 300))
  #expect(tl.y >= 50 && br.y <= 550 && abs((tl.x + br.x) / 2 - 400) < 0.5)
}

@Test func droppedNoteLandsUnderThePointerAtAnyZoom() {
  // The note's grabbed point moves by the pointer's translation: screen points / zoom = world units.
  for zoom in [0.2, 0.5, 1, 2.5, 3] {
    let from = CGPoint(x: 100, y: 40), by = CGSize(width: 60, height: -30)
    let to = GraphCanvas.droppedNoteOrigin(from: from, by: by, zoom: zoom)
    let cam = CanvasCamera(x: 40, y: 40, zoom: zoom)
    let before = cam.screen(from), after = cam.screen(to)
    // Within the rounding to whole world units.
    #expect(abs((after.x - before.x) - by.width) <= zoom / 2 + 1e-9)
    #expect(abs((after.y - before.y) - by.height) <= zoom / 2 + 1e-9)
  }
  #expect(GraphCanvas.droppedNoteOrigin(from: CGPoint(x: 10, y: 10), by: CGSize(width: 15, height: 7), zoom: 2) == CGPoint(x: 18, y: 14))
  // Not snapped to the grid.
  #expect(GraphCanvas.droppedNoteOrigin(from: .zero, by: CGSize(width: 13, height: 0), zoom: 1).x == 13)
}

@MainActor
@Test func editorSelectionAlwaysFitsTheNewText() {
  let tv = NSTextView()
  tv.string = "table Outputs { out:Num (required); }\nroot_type Outputs;\n"
  tv.setSelectedRange(NSRange(location: 40, length: 10))
  // A shorter text: no old range fits; an empty list would raise in AppKit.
  tv.string = "x"
  CodeEditor.select([NSRange(location: 40, length: 10)], in: tv)
  #expect(tv.selectedRange() == NSRange(location: 1, length: 0))
  // A range that partly fits is cut at the end of the text.
  tv.string = "hello world"
  CodeEditor.select([NSRange(location: 6, length: 50)], in: tv)
  #expect(tv.selectedRange() == NSRange(location: 6, length: 5))
  // No ranges at all: a cursor at the start.
  CodeEditor.select([], in: tv)
  #expect(tv.selectedRange() == NSRange(location: 0, length: 0))
  // UTF-16 lengths (an emoji is two units): never half a character.
  tv.string = "日本😀"
  CodeEditor.select([NSRange(location: 3, length: 9)], in: tv)
  #expect(tv.selectedRange() == NSRange(location: 2, length: 2))
}

@Test func treeSitterColorsEveryEditorLanguage() {
  func tokens(_ text: String, _ lang: String) -> Set<String> {
    let spans = TreeSitterHighlighter.spans(text, language: lang) ?? []
    return Set(spans.map { r, t in "\(t):" + (text as NSString).substring(with: r) })
  }
  let cc = tokens("// note\n#include <card.hh>\nstruct Out { int v; };\nauto run() -> int { return 1; }\n", "cc")
  #expect(cc.contains("comment:// note") && cc.contains("keyword:struct") && cc.contains("keyword:return"))
  let py = tokens("# note\ndef main(ctx):\n    return \"x\"\n", "py")
  #expect(py.contains("comment:# note") && py.contains("keyword:def") && py.contains("string:\"x\""))
  let nix = tokens("# note\n{ pkgs, ... }: let x = 1; in { a = \"s\"; }\n", "nix")
  #expect(nix.contains("comment:# note") && nix.contains("keyword:let") && nix.contains("number:1"))
  let fbs = tokens("// note\nnamespace t;\ntable Num { value:ulong (required); }\nroot_type Num;\n", "fbs")
  #expect(fbs.contains("comment:// note") && fbs.contains("keyword:table") && fbs.contains("type:Num") && fbs.contains("property:value"))
  #expect(TreeSitterHighlighter.spans("x", language: "txt") == nil)
}


@Test @MainActor func markdownTakesMathOutBeforeParsing() {
  let d = MarkdownDocument.make("""
  # Collatz
  $T(n) = n/2$ when *n* is even, and $3n+1$ otherwise; it costs $5 and $6.

  $$
  T(n) = \\begin{cases} n/2 & n \\equiv 0 \\\\ 3n+1 & n \\equiv 1 \\end{cases}
  $$

  `$x$` stays code.
  ```
  $not math$
  ```
  """)
  guard d.blocks.count == 5 else { Issue.record("blocks: \(d.blocks)"); return }
  #expect(d.blocks[0] == .heading(1, [.text("Collatz", [])]))
  if case .paragraph(let inl) = d.blocks[1] {
    #expect(inl.first == .math("T(n) = n/2"))
    #expect(inl.contains(.math("3n+1")))
    #expect(inl.contains(.text("n", [.emphasis])))
    #expect(!inl.contains { if case .math(let t) = $0 { return t.contains("5") } else { return false } })
  } else { Issue.record("paragraph expected") }
  if case .math(let tex) = d.blocks[2] { #expect(tex.hasPrefix("T(n) = \\begin{cases}")) } else { Issue.record("display math expected") }
  #expect(d.blocks[3] == .paragraph([.code("$x$"), .text(" stays code.", [])]))
  #expect(d.blocks[4] == .code("$not math$\n", nil))
  // TeX that cannot be typeset is shown as written.
  #expect(MathImages.image("\\frac{1}{2}", size: 13, display: false) != nil)
  #expect(MathImages.image("\\frac{1}{", size: 13, display: false) == nil)
  // Commands the typesetter lacks are written with ones it has.
  #expect(MathImages.image("T(n) = \\begin{cases} n/2 & n \\equiv 0 \\pmod 2 \\\\ 3n+1 & n \\equiv 1 \\pmod{2} \\end{cases}",
                           size: 13, display: true) != nil)
  #expect(MathImages.image("a \\quad b \\bmod 2", size: 13, display: false) != nil)
}

/// A new project goes into a new or empty folder, never over another.
@MainActor
@Test func aNewProjectNeverOverwritesAFolder() throws {
  let dir = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-new-\(UUID().uuidString)")
  defer { try? FileManager.default.removeItem(at: dir) }
  try AppModel.writeNewProject(at: dir, name: "first", flow: "leftToRight")
  #expect(FileManager.default.fileExists(atPath: dir.appendingPathComponent("tsugumi.json").path))
  #expect(throws: FolderNotEmpty.self) { try AppModel.writeNewProject(at: dir, name: "second", flow: "leftToRight") }
  let doc = try String(contentsOf: dir.appendingPathComponent("tsugumi.json"), encoding: .utf8)
  #expect(doc.contains("first") && !doc.contains("second"))
}
