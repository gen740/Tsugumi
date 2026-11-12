import Foundation
import AppKit
import SwiftUI
import Testing
@testable import TsugumiLauncher
@testable import TsugumiApp
@testable import TsugumiRPC

@Test func nixSourcesNameTheirFlakesAsTheWebUIDoes() throws {
  struct Case: Decodable { let ref: String; let name: String; let unlocked: String; let commit: String? }
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../..")
  let cases = try JSONDecoder().decode([Case].self, from: Data(contentsOf: root.appendingPathComponent("tests/fixtures/nix-source-refs.json")))
  #expect(!cases.isEmpty)
  for c in cases {
    #expect(NixSources.sourceName(c.ref) == c.name, "\(c.ref)")
    #expect(NixSources.unlockedRef(c.ref) == c.unlocked, "\(c.ref)")
    #expect(NixSources.shortCommit(c.ref) == c.commit, "\(c.ref)")
  }
}

@Test func aVersionHoverIsWrittenFromItsChangeAndFitsItsCards() {
  #expect(VersionSummarizer.input(message: "実行: bands\n", changed: ["bands", ""], diff: "+x\n", truncated: true)
    == "Commit message: 実行: bands\nChanged Cards: bands, the Workflow itself\n\nDiff (cut short):\n+x\n")
  #expect(VersionSummarizer.clean(" 緑を\n  広げた。 ") == "緑を 広げた。")
  // Every Card stays inside the box, in the order it has on the canvas.
  let wide = VersionMiniMapLayout([.init(id: "a", x: 100, y: 0), .init(id: "b", x: 5000, y: 300), .init(id: "c", x: 2500, y: -400)],
                                  flow: .leftToRight, width: 284, height: 132)
  for (_, c) in wide.cards {
    let r = c.box
    #expect(r.minX >= 0 && r.maxX <= 284 && r.minY >= 0 && r.maxY <= wide.height && wide.height <= 132)
  }
  #expect(wide.cards["a"]!.box.minX < wide.cards["c"]!.box.minX && wide.cards["c"]!.box.minX < wide.cards["b"]!.box.minX)
  #expect(!wide.named)
  // A small graph is not blown up, and is only as tall as it is.
  let small = VersionMiniMapLayout([.init(id: "a", x: 0, y: 0)], flow: .leftToRight, width: 284, height: 132)
  #expect(small.cards["a"]!.box.width == 220 * 0.3 && small.height == small.cards["a"]!.box.height + 16 && small.named)
  #expect(VersionMiniMapLayout([], flow: .leftToRight, width: 284, height: 132).cards.isEmpty)
}

@Test func aVersionsMinimapDrawsWhatIsNotACardWithItsCanvasShape() {
  func canvas(_ kind: CardKind, fan: Int = 0, flow: FlowDirection = .leftToRight) -> CardGeometry {
    CardGeometry(VersionMiniMapLayout.Card(id: "c", x: 0, y: 0, kind: kind, fan: fan).data, flow: flow)
  }
  func outline(_ g: CardGeometry, in box: CGRect, _ k: CGFloat) -> Path {
    let o = g.outlineRect
    return g.outline(in: CGRect(x: box.minX + o.minX * k, y: box.minY + o.minY * k, width: o.width * k, height: o.height * k), cornerScale: k)
  }
  // A graph laid out on the canvas does not overlap here: start -> fork -> two Cards.
  let start = canvas(.trigger), fork = canvas(.fork, fan: 2)
  let forked = VersionMiniMapLayout([
    .init(id: "start", x: 0, y: 30, kind: .trigger),
    .init(id: "fork", x: start.size.width + 10, y: 20, kind: .fork, fan: 2),
    .init(id: "load", x: start.size.width + fork.size.width + 40, y: -30),
    .init(id: "draft", x: start.size.width + fork.size.width + 40, y: 60, kind: .draft),
  ], flow: .leftToRight, width: 284, height: 132)
  let at = forked.cards
  #expect(at["start"]!.shape == .start && at["fork"]!.shape == .fork && at["load"]!.shape == .card && at["draft"]!.shape == .card)
  #expect(at["draft"]!.draft && !at["load"]!.draft)
  let boxes = Array(at.values.map(\.box))
  for (i, a) in boxes.enumerated() {
    for b in boxes[(i + 1)...] { #expect(!a.intersects(b)) }
  }
  // The shapes are those of the canvas, scaled and at their place.
  let k = at["fork"]!.box.width / fork.size.width
  #expect(k > 0 && k <= 0.3)
  #expect(abs(at["fork"]!.box.height - fork.size.height * k) < 1e-9 && abs(at["start"]!.box.width - start.size.width * k) < 1e-9)
  #expect(at["fork"]!.outline == outline(fork, in: at["fork"]!.box, k))
  #expect(at["start"]!.outline == outline(start, in: at["start"]!.box, k))
  #expect(abs(at["fork"]!.outline.boundingRect.minX - (at["fork"]!.box.minX + fork.outlineRect.minX * k)) < 1)

  // A branch is a diamond whose corners shrink with it; a merge is a triangle that
  // grows with its inputs, and a fork with its outputs; both follow the flow.
  let diamond = VersionMiniMapLayout([.init(id: "b", x: 0, y: 0, kind: .branch)], flow: .leftToRight, width: 284, height: 132).cards["b"]!
  #expect(diamond.shape == .branch && abs(diamond.box.width - canvas(.branch).size.width * 0.3) < 1e-9)
  #expect(diamond.outline == outline(canvas(.branch), in: diamond.box, 0.3))
  #expect(diamond.outline != outline(canvas(.branch), in: diamond.box, 1))
  func single(_ kind: CardKind, _ fan: Int, _ flow: FlowDirection = .leftToRight) -> VersionMiniMapLayout.Placed {
    VersionMiniMapLayout([.init(id: "m", x: 0, y: 0, kind: kind, fan: fan)], flow: flow, width: 284, height: 132).cards["m"]!
  }
  #expect(single(.merge, 2).shape == .merge)
  #expect(single(.merge, 6).box.height > single(.merge, 2).box.height)
  #expect(single(.merge, 6, .topToBottom).box.width > single(.merge, 2, .topToBottom).box.width)
  #expect(single(.merge, 2).outline != single(.merge, 2, .topToBottom).outline)
  #expect(single(.fork, 6).box.height > single(.fork, 2).box.height)

  // A loop is a merge and a branch with a wire back: they keep their shapes and do not overlap.
  let loop = VersionMiniMapLayout([.init(id: "m", x: 0, y: 0, kind: .merge, fan: 2), .init(id: "step", x: 140, y: 0), .init(id: "b", x: 400, y: 0, kind: .branch)],
                                  flow: .leftToRight, width: 284, height: 132).cards
  #expect(loop["m"]!.shape == .merge && loop["step"]!.shape == .card && loop["b"]!.shape == .branch)
  #expect(!loop["m"]!.box.intersects(loop["step"]!.box) && !loop["step"]!.box.intersects(loop["b"]!.box))

  // A folded Card is its header.
  let folded = VersionMiniMapLayout([.init(id: "a", x: 0, y: 0, collapsed: true)], flow: .leftToRight, width: 284, height: 132).cards["a"]!
  #expect(abs(folded.box.height - CardGeometry.header * 0.3) < 1e-9)
}

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

/// A wire to or from a Draft is dotted until the Draft has code. The web checks
/// the same (web/tests/draftWires.tsx).
@MainActor @Test func draftWiresAreDotted() throws {
  let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("draft-wires-\(UUID().uuidString.prefix(8))")
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: dir) }
  let document = #"{"name": "t", "canvas": {"cards": [], "connections": []}}"#
  try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  s.project = try JSONValue.parse(document)
  let start = s.addCard(template: .trigger, in: [], at: .zero, name: "start")
  let log = s.addCard(template: .logger, in: [], at: CGPoint(x: 600, y: 0), name: "log")
  let draft = s.addCard(template: .draft, in: [], at: CGPoint(x: 300, y: 0), name: "plan")

  #expect(s.isDraftWire(from: draft, to: log), "from a Draft")
  #expect(s.isDraftWire(from: start, to: draft), "to a Draft")
  #expect(s.isDraftWire(from: draft, to: draft), "between Drafts")
  #expect(s.isDraftWire(from: draft, to: nil), "from a Draft to the bar of a Card in Card")
  #expect(!s.isDraftWire(from: start, to: log), "between Cards with code")
  #expect(!s.isDraftWire(from: start, to: nil), "from a Card with code to the bar")

  // Code written for the Draft: its wires are solid again, without any other change.
  s.updateCard(draft) { (card: inout JSONValue) in card = .object([("language", .string("python"))]) }
  #expect(s.kind(draft) == .code)
  #expect(!s.isDraftWire(from: draft, to: log))
  #expect(!s.isDraftWire(from: start, to: draft))

  // The dots flow with the active wires' phase, which wraps at one dash period.
  let (on, off) = (WireLayer.dot[0], WireLayer.dot[1])
  #expect(on == 0, "round dots")
  #expect(off > 0)
  let dots = WireLayer.dash.reduce(0, +) / off
  #expect(abs(dots - dots.rounded()) < 1e-9, "a whole number of dots per period")
}

/// The thumbnails decide the same from a demo's Cards, whatever it is drawing.
@MainActor @Test func aThumbnailDotsADraftsWires() throws {
  let card = { (id: String, kind: String) in
    "{\"id\": \"\(id)\", \"kind\": \"\(kind)\", \"x\": 0, \"y\": 0, \"inputs\": [{\"name\": \"in\"}], \"outputs\": [{\"name\": \"out\"}], \"params\": [], \"permissions\": []}"
  }
  let wire = { (a: String, b: String) in "{\"from\": \"\(a)\", \"fromPort\": \"out\", \"to\": \"\(b)\", \"toPort\": \"in\"}" }
  let json = """
    {"flow": "topToBottom", "draftOnly": true, "frames": {}, "steps": [], "final": {}, "outputs": {},
     "cards": [\(card("a", "code")), \(card("b", "code")), \(card("d", "draft"))],
     "wires": [\(wire("a", "b")), \(wire("b", "d")), \(wire("d", "a"))]}
    """
  let scene = DemoScene(try JSONDecoder().decode(DemoPlayerData.self, from: Data(json.utf8)))
  #expect(scene.wires.map(\.dotted) == [false, true, true])
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

@Test func wiresBetweenClosePortsTakeTheShortWay() {
  func length(_ path: Path) -> CGFloat {
    let points = stride(from: 0.0, through: 1.0, by: 1.0 / 64).map { path.trimmedPath(from: 0, to: max($0, 0.0001)).currentPoint ?? .zero }
    return zip(points, points.dropFirst()).reduce(0) { $0 + hypot($1.1.x - $1.0.x, $1.1.y - $1.0.y) }
  }
  let merge = CGRect(x: 0, y: 0, width: 80, height: 50)
  for gap in [0, 1, 20, 23, 24, 25, 40] as [CGFloat] {
    let card = CGRect(x: -70, y: 50 + gap, width: 220, height: 90)
    var down = WireRouter(flow: .topToBottom, obstacles: [merge, card])
    #expect(length(down.route(CGPoint(x: 40, y: 50), CGPoint(x: 40, y: 50 + gap))) <= gap + 0.5, "top to bottom, \(gap)")
  }
  var across = WireRouter(flow: .leftToRight, obstacles: [CGRect(x: 0, y: 0, width: 50, height: 80), CGRect(x: 70, y: -70, width: 90, height: 220)])
  #expect(length(across.route(CGPoint(x: 50, y: 40), CGPoint(x: 70, y: 46))) < 25)
  // No bulge past either port.
  var offset = WireRouter(flow: .topToBottom, obstacles: [merge])
  let bent = offset.route(CGPoint(x: 40, y: 50), CGPoint(x: 52, y: 70)).boundingRect
  #expect(bent.minY >= 49.5 && bent.maxY <= 70.5)
  // Upstream by a point is still upstream: it goes around.
  var back = WireRouter(flow: .topToBottom, obstacles: [merge, CGRect(x: -70, y: 70, width: 220, height: 90)])
  #expect(length(back.route(CGPoint(x: 40, y: 51), CGPoint(x: 40, y: 50))) > 50)
  // The short way through a Card is not taken.
  let wall = CGRect(x: 100, y: 45, width: 60, height: 30)
  var blocked = WireRouter(flow: .topToBottom, obstacles: [merge, wall])
  #expect(!blocked.isForward(CGPoint(x: 40, y: 50), CGPoint(x: 220, y: 60)))
  let around = blocked.route(CGPoint(x: 40, y: 50), CGPoint(x: 220, y: 60))
  for t in stride(from: 0.02, through: 0.98, by: 0.02) {
    #expect(!wall.insetBy(dx: 2, dy: 2).contains(around.trimmedPath(from: 0, to: t).currentPoint ?? .zero))
  }
}

/// The paths the web finds for the same wires (web/tests/wireDemos.ts writes the file):
/// real layouts of the demos, so the two routers cannot drift apart.
@Test func wiresRouteAsOnTheWeb() throws {
  struct Point: Decodable { let x, y: CGFloat }
  struct Rect: Decodable { let x, y, width, height: CGFloat }
  struct Ends: Decodable { let a, b: Point }
  struct Case: Decodable {
    let name, flow: String
    let obstacles: [Rect]
    let wires: [Ends]
    let paths: [[Point]?]
  }
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../..")
  let cases = try JSONDecoder().decode([Case].self, from: Data(contentsOf: root.appendingPathComponent("tests/fixtures/wire-routes.json")))
  #expect(!cases.isEmpty)
  for c in cases {
    var router = WireRouter(flow: c.flow == "leftToRight" ? .leftToRight : .topToBottom,
                            obstacles: c.obstacles.map { CGRect(x: $0.x, y: $0.y, width: $0.width, height: $0.height) })
    let plans = router.plan(c.wires.map { (a: CGPoint(x: $0.a.x, y: $0.a.y), b: CGPoint(x: $0.b.x, y: $0.b.y)) })
    for (i, plan) in plans.enumerated() {
      let expected = c.paths[i]?.map { CGPoint(x: $0.x, y: $0.y) }
      #expect(plan == expected, "\(c.name), wire \(i)")
    }
  }
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

/// The same numbers as the web (tests/fixtures/port-grid-contract.json, web/tests/portGrid.ts).
@Test func portsFollowTheGridAsOnTheWeb() throws {
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../..")
  let data = try Data(contentsOf: root.appendingPathComponent("tests/fixtures/port-grid-contract.json"))
  let contract = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
  for c in try #require(contract["snap"] as? [[String: Double]]) {
    #expect(abs(snapToGrid(CGFloat(c["v"]!), CGFloat(c["grid"]!)) - CGFloat(c["to"]!)) < 1e-9)
  }
  for c in try #require(contract["ports"] as? [[String: Any]]) {
    let name = "\(c["flow"]!) \(c["kind"]!) grid \(c["grid"]!) \(c["inputs"]!)/\(c["outputs"]!)"
    func ports(_ count: Int) -> [PortData] { (0..<count).map { PortData(name: "p\($0)") } }
    var d = CardNodeData(title: "x", kind: try #require(CardKind(rawValue: c["kind"] as! String)), summary: c["summary"] != nil ? "s" : nil,
                         inputs: ports(c["inputs"] as! Int), outputs: ports(c["outputs"] as! Int))
    d.collapsed = c["collapsed"] != nil
    let grid = c["grid"] as? Double
    d.portGrid = grid.map { CGFloat($0) }
    let flow: FlowDirection = c["flow"] as! String == "topToBottom" ? .topToBottom : .leftToRight
    let g = CardGeometry(d, flow: flow)
    let width = c["width"] as! Double, height = c["height"] as! Double
    #expect(abs(g.size.width - width) < 1e-5, "\(name) width \(g.size.width)")
    #expect(abs(g.size.height - height) < 1e-5, "\(name) height \(g.size.height)")
    let edge = flow == .topToBottom ? width : height
    if d.kind == .fork || d.kind == .merge {
      let r = g.outlineRect
      for z: CGFloat in [1, 0.25] {
        // What is drawn: every port is on it, and rounding takes only a little off the wide edge.
        let outline = g.outline(in: CGRect(x: r.minX * z, y: r.minY * z, width: r.width * z, height: r.height * z))
        let line = outline.strokedPath(StrokeStyle(lineWidth: 2 * z))
        for (i, output) in (0..<d.inputs.count).map({ ($0, false) }) + (0..<d.outputs.count).map({ ($0, true) }) {
          let p = g.port("p\(i)", output: output)
          #expect(line.contains(CGPoint(x: p.x * z, y: p.y * z)), "\(name) port \(i) output \(output) on the outline at \(z)")
        }
        let box = outline.boundingRect
        #expect((flow == .topToBottom ? box.width : box.height) >= edge * z - 8 * z, "\(name) keeps its width at \(z)")
      }
      // Many ports make it bigger, not flatter.
      #expect((flow == .topToBottom ? r.height : r.width) >= edge / 4 - 1e-9, "\(name) not flat")
      let icon = g.iconCenter(in: r)
      #expect(abs((flow == .topToBottom ? icon.x : icon.y) - (flow == .topToBottom ? r.midX : r.midY)) < 1e-5, "\(name) icon on the axis")
    }
    for (key, output) in [("in", false), ("out", true)] {
      let list = c[key] as! [Double]
      let apex = (d.kind == .fork && !output) || (d.kind == .merge && output)
      for (i, v) in list.enumerated() {
        let p = g.port("p\(i)", output: output)
        #expect(abs((flow == .topToBottom ? p.x : p.y) - v) < 1e-5, "\(name) \(key) \(i): \(p) != \(v)")
        guard let grid, let n = c["n"] as? Double else { continue }
        // A Card's corner is on a grid line, so this is its place on the canvas too.
        let lines = v * n / grid
        #expect(abs(lines - lines.rounded()) < 1e-5, "\(name) \(key) \(i) on grid/\(n)")
        #expect(v >= 0 && v <= edge, "\(name) on its edge")
        // A triangle's apex takes everything on that side; elsewhere marks never touch.
        if apex { #expect(abs(v - edge / 2) < 1e-5, "\(name) at the apex") }
        else if i > 0, c["collapsed"] == nil || flow == .topToBottom { #expect(v - list[i - 1] >= CardGeometry.minPortPitch, "\(name) apart") }
      }
    }
  }
}

/// Counts the requests to redraw the whole column.
private final class CountingRuler: LineNumberRuler {
  var wholeRedraws = 0
  override var needsDisplay: Bool {
    get { super.needsDisplay }
    set {
      if newValue { wholeRedraws += 1 }
      super.needsDisplay = newValue
    }
  }
}

/// AppKit may ask the line numbers for more than the column's height and then
/// count all of it as drawn, so a scroll alone would leave the rest blank.
@MainActor @Test func scrollingTheEditorRedrawsTheWholeLineNumberColumn() {
  let scroll = NSTextView.scrollableTextView()
  scroll.frame = NSRect(x: 0, y: 0, width: 400, height: 300)
  let tv = scroll.documentView as! NSTextView
  tv.string = (1...100).map { "line \($0)" }.joined(separator: "\n")
  scroll.hasVerticalRuler = true
  scroll.rulersVisible = true
  let ruler = CountingRuler(textView: tv)
  scroll.verticalRulerView = ruler
  tv.layoutManager?.ensureLayout(for: tv.textContainer!)
  ruler.wholeRedraws = 0

  scroll.contentView.scroll(to: NSPoint(x: 0, y: 100))
  #expect(ruler.wholeRedraws > 0)
}

@Test func theChatTakesItsWidthFromWhatTheWorkbenchLeaves() {
  #expect(ChatSplit.chatWidth(480, in: 1220) == 480)
  // What was asked for stays within the chat's limits.
  #expect(ChatSplit.chatWidth(100, in: 1220) == 320)
  #expect(ChatSplit.chatWidth(2000, in: 2000) == 900)
  // The workbench keeps its minimum, in a narrow window too.
  #expect(ChatSplit.chatWidth(480, in: 700) == 340)
  #expect(ChatSplit.chatWidth(480, in: 500) == 140)
  #expect(ChatSplit.chatWidth(480, in: 200) == 0)
}

@MainActor @Test func rubyCardScaffoldIsEditableAndUndoable() async throws {
  let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("ruby-card-\(UUID())")
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: dir) }
  let document = #"{"name":"ruby","canvas":{"cards":[],"connections":[]}}"#
  try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  session.project = try JSONValue.parse(document)
  #expect(BuiltinCardItems.written.contains(.ruby))  // offered in the canvas's "Add Card" menu
  let card = session.addCard(template: .ruby, in: [], at: .zero, name: "ruby")
  #expect(session.definitionDrafts[card]?.card["language"]?.string == "ruby")
  #expect(try await session.sourceText("definitions/\(card)/default.nix").contains("rubyCard"))
  #expect(try await session.sourceText("definitions/\(card)/src/main.rb").contains("def main(ctx)"))
  session.undo()
  #expect(session.definitionDrafts[card] == nil)
}
