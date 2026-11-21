import Foundation
import Testing
import SwiftUI
@testable import TsugumiApp
@testable import TsugumiRPC

@Suite struct PresenceTests {
@Test func toolCallsTellWhichCardTheAgentLooksAt() {
  #expect(PresenceEvents.look(tool: "project_file_read", arguments: #"{"path": "definitions/bands/src/main.py"}"#) == .visit(["bands"]))
  #expect(PresenceEvents.look(tool: "run", arguments: #"{"card": ["start"], "propagate": true}"#) == .run(["start"]))
  #expect(PresenceEvents.look(tool: "run_record", arguments: #"{"run_id": "r", "card": ["rgb"]}"#) == .visit(["rgb"]))
  // Not a Card's file, nor a tool that looks at one.
  #expect(PresenceEvents.look(tool: "project_file_read", arguments: #"{"path": "tsugumi.json"}"#) == nil)
  #expect(PresenceEvents.look(tool: "run", arguments: "not json") == nil)
  // The CardPath as the model writes it (the tools' schema), also in Card.
  #expect(PresenceEvents.look(tool: "run", arguments: #"{"base_rev": "3", "card": {"segments": ["start"]}}"#) == .run(["start"]))
  #expect(PresenceEvents.look(tool: "run_record", arguments: #"{"run_id": "r", "card": {"segments": ["collatz-cpp", "check"]}}"#) == .visit(["collatz-cpp", "check"]))
  #expect(PresenceEvents.look(tool: "log_range", arguments: #"{"run_id": "r", "card": {"segments": []}}"#) == nil)
}


@MainActor @Test func theLastWordsSayHowTheTurnEnded() {
  let t0 = Date(timeIntervalSinceReferenceDate: 1000)
  for (outcome, words) in [(AgentPresence.Outcome.done, "できました"), (.failed, "うまくいきませんでした"),
                           (.stopped, "止めました")] {
    let p = AgentPresence(animateWork: { true })
    p.turnStarted(now: t0)
    p.enqueue(.visit("load"), now: t0)
    p.turnEnded(outcome, now: t0.addingTimeInterval(0.5))
    let end = p.beats.last!.end
    #expect(p.line(at: end.addingTimeInterval(0.5))?.text == words)
    // A thought summary that comes after the end is not shown.
    p.think("帯域を分けています", now: end.addingTimeInterval(0.6))
    #expect(p.line(at: end.addingTimeInterval(0.7))?.text == words)
  }
  // A turn without scenes (a question answered) ends with its words too.
  let q = AgentPresence(animateWork: { true })
  q.turnStarted(now: t0)
  q.turnEnded(now: t0.addingTimeInterval(2))
  #expect(q.line(at: t0.addingTimeInterval(2.5))?.text.isEmpty == false)
}

@Test func aSaveWritesOutDraftsUpstreamFirstAndThreadsTheirWires() {
  typealias C = PresenceEvents.Connection
  let wiring = [C(from: "start", fromPort: "out", to: "load", toPort: "start"),
                C(from: "load", fromPort: "events", to: "bands", toPort: "input")]
  let x = ["start": 0.0, "load": 200, "bands": 500]
  let w = PresenceEvents.written(
    upserted: ["bands", "load"], touchedSources: ["load"], existedBefore: ["start", "load", "bands"],
    typeBefore: ["start": "trigger", "load": "draft", "bands": "draft"],
    typeAfter: ["start": "trigger", "bands": "draft"],
    cards: ["start", "load", "bands"], connectionsBefore: wiring, connectionsAfter: wiring, order: { x[$0] ?? 0 })
  // load: a Draft that became code (written out); bands: still a Draft (only touched).
  #expect(w.cards.map(\.0) == ["load", "bands"])
  #expect(w.cards.map(\.reveal) == [true, false])
  // Existing wires are not newly connected by a source edit.
  #expect(w.wires.isEmpty)

  // A wire the save adds is threaded too; Cards in Card and unknown ids are left out.
  let added = C(from: "bands", fromPort: "images", to: "rgb", toPort: "input")
  let w2 = PresenceEvents.written(
    upserted: ["rgb", "inner"], touchedSources: [], existedBefore: ["rgb"], typeBefore: [:], typeAfter: [:],
    cards: ["bands", "rgb"], connectionsBefore: wiring, connectionsAfter: wiring + [added], order: { _ in 0 })
  #expect(w2.cards.map(\.0) == ["rgb"] && w2.cards.map(\.reveal) == [false])
  #expect(w2.wires == [added])
}




@Test func thoughtLinesAreOneShortLine() {
  #expect(ThoughtSummarizer.clean("「帯域の境界を広げる」\n二行目") == "帯域の境界を広げる")
  #expect(ThoughtSummarizer.clean("  \n") == nil)
  let shortened = ThoughtSummarizer.clean(String(repeating: "あ", count: 100)) ?? ""
  #expect(!shortened.isEmpty && shortened.count < 100)
}

@MainActor @Test func theBirdFadesInAndAwayInsteadOfPopping() {
  let p = AgentPresence(animateWork: { true })
  let t0 = Date(timeIntervalSinceReferenceDate: 1000)
  let T = AgentPresence.Timing.self
  p.turnStarted(now: t0)
  p.enqueue(.visit("load"), now: t0)
  #expect(p.fade(at: t0).opacity == 0)
  #expect(p.fade(at: t0.addingTimeInterval(T.fadeIn)).opacity == 1)
  let end = t0.addingTimeInterval(T.fadeIn)
  p.turnEnded(now: end)
  // Still fully there, the done line said and then gone before the bird.
  #expect(p.fade(at: end).opacity == 1)
  #expect(p.line(at: end)?.text.isEmpty == false)
  #expect(p.line(at: end.addingTimeInterval((T.doneLine + T.linger) / 2)) == nil)
  // Fading and rising over the last moment of its stay.
  let mid = p.fade(at: end.addingTimeInterval(T.linger - T.fadeOut / 2))
  #expect(mid.opacity > 0 && mid.opacity < 1 && mid.lift > 0)
  #expect(p.fade(at: end.addingTimeInterval(T.linger)).opacity == 0)
}

@MainActor @Test func theFileBeingWrittenShowsReceivedTextImmediately() {
  let p = AgentPresence(animateWork: { true })
  let start = Date(timeIntervalSinceReferenceDate: 1000)
  p.turnStarted(now: start)
  p.toolInput(index: 0, tool: "project_save", delta: #"{"changes":[{"value":{"path":"definitions/a/main.py","bytes":"import os"#, now: start)
  #expect(p.liveCode(at: start)?.text == "import os")
  #expect(p.liveCode(at: start.addingTimeInterval(0.2))?.text == "import os")
  p.toolInput(index: 0, tool: "project_save", delta: "\\nprint", now: start.addingTimeInterval(0.3))
  #expect(p.liveCode(at: start.addingTimeInterval(0.3))?.text == "import os\nprint")
  p.toolInput(index: 0, tool: "project_save", delta: #""}},{"value":{"path":"definitions/b/main.py","patches":[{"text":"replacement"#, now: start.addingTimeInterval(0.4))
  let patch = p.liveCode(at: start.addingTimeInterval(0.4))
  #expect(patch?.card == "b" && patch?.patch == true && patch?.text == "replacement")
  p.toolInput(index: 0, tool: "project_save", delta: #""},{"text":""#, now: start.addingTimeInterval(0.5))
  #expect(p.liveCode(at: start.addingTimeInterval(0.5))?.text == "")
  p.toolInput(index: 0, tool: "project_save", delta: #""}]}}]}"#, now: start.addingTimeInterval(0.6))
  p.saved(written: [("b", reveal: false)], wires: [], now: start.addingTimeInterval(0.7))
  p.toolInputEnded(ok: true, now: start.addingTimeInterval(0.7))
  #expect(p.liveCode(at: start.addingTimeInterval(0.8))?.saved == true)
  #expect(p.liveCode(at: start.addingTimeInterval(1.5)) == nil)
}


@MainActor @Test func aNewFileReplacesAnOlderFileAtOnce() {
  let p = AgentPresence(animateWork: { true })
  let t0 = Date(timeIntervalSinceReferenceDate: 1000)
  let at = { (s: Double) in t0.addingTimeInterval(s) }
  let file = { (card: String) in #"{"value":{"path":"definitions/\#(card)/m.py","bytes":"code"}},"# }
  p.turnStarted(now: t0)
  p.toolInput(index: 0, tool: "project_save", delta: file("a"), now: t0)
  #expect(p.liveCode(at: at(0.1))?.card == "a")
  // No shortest stay: the file arriving now is the one on the screen.
  p.toolInput(index: 0, tool: "project_save", delta: file("b"), now: at(0.2))
  #expect(p.liveCode(at: at(0.2))?.card == "b")
  #expect(p.beats.map(\.beat) == [.compose("a"), .compose("b")])
  #expect(p.beats[0].end == at(0.2))
}



@MainActor @Test func aStoppedTurnOrAnUnansweredApprovalLeavesNoUnsavedText() {
  typealias T = AgentPresence.Timing
  let p = AgentPresence(animateWork: { true })
  let t0 = Date(timeIntervalSinceReferenceDate: 1000)
  let at = { (s: Double) in t0.addingTimeInterval(s) }
  // Stopped while the file still arrives.
  p.turnStarted(now: t0)
  p.toolInput(index: 0, tool: "project_save", delta: #"{"value":{"path":"definitions/a/m.py","bytes":"half"#, now: t0)
  #expect(p.liveCode(at: at(0.2))?.text == "half")
  p.turnEnded(.stopped, now: at(0.3))
  #expect(p.liveCode(at: at(0.3)) == nil && p.liveCode(at: at(2)) == nil)
  // All of it has arrived and the save waits for an approval: it is never
  // marked as saved, it is gone after its hold, and a refusal takes it away at once.
  p.turnStarted(now: at(10))
  p.toolInput(index: 1, tool: "project_save", delta: #"{"value":{"path":"definitions/a/m.py","bytes":"whole"}}"#, now: at(10))
  #expect(p.liveCode(at: at(10.1))?.saved == false)
  #expect(p.liveCode(at: at(10 + T.codeHold + 0.01)) == nil)
  p.toolInput(index: 2, tool: "project_save", delta: #"{"value":{"path":"definitions/a/m.py","bytes":"again"}}"#, now: at(20))
  p.toolInputEnded(ok: false, now: at(20.1))
  #expect(p.liveCode(at: at(20.1)) == nil)
}

@MainActor @Test func helpersWritingTheSameFileEachShowTheirOwnText() {
  let t0 = Date(timeIntervalSinceReferenceDate: 1000)
  let birds = ["first", "second"].map { (text: String) -> AgentPresence in
    let p = AgentPresence(animateWork: { true })
    p.turnStarted(now: t0)
    p.toolInput(index: 0, tool: "project_save", delta: #"{"value":{"path":"definitions/a/m.py","bytes":"\#(text)"#, now: t0)
    return p
  }
  #expect(birds.map { $0.liveCode(at: t0)?.text } == ["first", "second"])
  #expect(birds.map { $0.liveCode(at: t0)?.card } == ["a", "a"])
  // One fails: only its panel goes.
  birds[0].toolInputEnded(ok: false, now: t0)
  #expect(birds[0].liveCode(at: t0) == nil && birds[1].liveCode(at: t0)?.text == "second")
}

@Test func thePanelIsHiddenWithTheAgentAndWhereItCouldNotBeRead() {
  #expect(WritingPanel.shows(avatar: .bird, zoom: 1))
  #expect(WritingPanel.shows(avatar: .dot, zoom: WritingPanel.minimumZoom))
  #expect(!WritingPanel.shows(avatar: .bird, zoom: 0.4))
  #expect(!WritingPanel.shows(avatar: .none, zoom: 1))
}

@Test func thePanelOfACardNotOnTheCanvasGoesWhereThereIsRoom() {
  let view = CGRect(x: 12, y: 52, width: 1000, height: 800)
  let w = WritingPanel.size.width, h = WritingPanel.size.height
  // The top middle of the view.
  let top = WritingPanel.frame(scale: 1, in: view)
  #expect(top == CGRect(x: view.midX - w / 2, y: view.minY, width: w, height: h))
  // Something there: the bottom middle.
  let bottom = WritingPanel.frame(scale: 1, in: view, others: [top])
  #expect(bottom == CGRect(x: view.midX - w / 2, y: view.maxY - h, width: w, height: h))
  // Both: a corner, whole and covering nothing.
  let corner = WritingPanel.frame(scale: 1, in: view, others: [top, bottom])
  #expect(view.contains(corner) && !corner.intersects(top) && !corner.intersects(bottom))
  // Nothing free: the first place all the same, inside the view.
  #expect(WritingPanel.frame(scale: 1, in: view, others: [view]) == top)
  // Another bird does not take the same place, and it scales as the others.
  #expect(WritingPanel.frame(scale: 1, in: view, slot: 1) == bottom)
  #expect(WritingPanel.frame(scale: 1.3, in: view).height == h * 1.3)
}

@MainActor @Test func whatAFailedOrStoppedCallWroteGoesAtOnce() {
  let p = AgentPresence(animateWork: { true })
  let t0 = Date(timeIntervalSinceReferenceDate: 1000)
  let at = { (s: Double) in t0.addingTimeInterval(s) }
  let file = { (card: String) in #"{"value":{"path":"definitions/\#(card)/m.py","bytes":"code"}},"# }
  p.turnStarted(now: t0)
  p.toolInput(index: 0, tool: "project_save", delta: file("a"), now: t0)
  p.toolInput(index: 0, tool: "project_save", delta: file("b"), now: t0)
  #expect(p.beats.count <= 2)
  p.toolInputEnded(ok: false, now: at(0.5))
  #expect(p.liveCode(at: at(0.5)) == nil && p.liveCode(at: at(4)) == nil)
  // What is not a Card's file shows nothing, and the next call starts afresh.
  p.toolInput(index: 1, tool: "project_save", delta: #"{"changes":[{"value":{"path":"layout.json","bytes":"{"#, now: at(1))
  #expect(p.liveCode(at: at(1)) == nil)
  p.toolInput(index: 2, tool: "project_save", delta: file("c"), now: at(2))
  #expect(p.liveCode(at: at(2.5))?.card == "c")
  // A turn that is stopped leaves nothing behind; one that ends well plays out.
  p.turnEnded(.stopped, now: at(3))
  #expect(p.liveCode(at: at(3)) == nil)
  p.turnStarted(now: at(10))
  p.toolInput(index: 3, tool: "project_save", delta: file("d"), now: at(10))
  p.turnEnded(.done, now: at(10.1))
  #expect(p.liveCode(at: at(10.2))?.card == "d")
}

@Test func thePanelShowsTheLastLines() {
  #expect(WritingPanel.tail("a\nb", lines: 5) == ["a", "b"])
  #expect(WritingPanel.tail("1\n2\n3\n4\n5\n6\n7", lines: 3) == ["5", "6", "7"])
  // A new line just begun is kept, so the text moves as it is written.
  #expect(WritingPanel.tail("1\n2\n3\n", lines: 3) == ["2", "3", ""])
  #expect(WritingPanel.tail("", lines: 3) == [""])
  // CR LF ends a line too, and a tab does not take a tab stop of its own.
  #expect(WritingPanel.tail("a\r\nb\n\tc", lines: 3) == ["a", "b", "    c"])
}

@Test func thePanelStaysClearOfTheBirdAndInsideTheView() {
  let view = CGRect(x: 12, y: 52, width: 1000, height: 800)
  let card = CGRect(x: 400, y: 400, width: 220, height: 120)
  let w = WritingPanel.size.width, h = WritingPanel.size.height
  // Above the bird and the bubble (the 80 points over the Card), centred on the Card.
  let above = WritingPanel.frame(card: card, clear: 80, scale: 1, in: view)!
  #expect(above.maxY <= card.minY - 80)
  #expect(above.midX == card.midX && above.width == w && above.height == h)
  // No room above: under the Card, away from the bird and the bubble.
  let top = card.offsetBy(dx: 0, dy: -300)
  let under = WritingPanel.frame(card: top, clear: 80, scale: 1, in: view)!
  #expect(under.minY >= top.maxY && view.contains(under))
  // Something above it (another row of Cards) and nothing under: under the Card.
  let row = card.offsetBy(dx: 0, dy: -200)
  let moved = WritingPanel.frame(card: card, clear: 80, scale: 1, in: view, others: [row])!
  #expect(moved.minY >= card.maxY && !moved.intersects(row))
  // Something on both sides: above the bird and the bubble all the same, never over them.
  let both = WritingPanel.frame(card: card, clear: 80, scale: 1, in: view, others: [row, card.offsetBy(dx: 0, dy: 200)])!
  #expect(both.maxY <= card.minY - 80)
  // Half out of the view on a side: still whole.
  for dx in [-500.0, 500] {
    let out = card.offsetBy(dx: dx, dy: 0)
    #expect(out.intersects(view))
    #expect(view.contains(WritingPanel.frame(card: out, clear: 80, scale: 1, in: view)!))
  }
  // No room above or under (a Card as tall as the view): whole, on the roomier side.
  let tall = CGRect(x: 400, y: 100, width: 220, height: 700)
  #expect(view.contains(WritingPanel.frame(card: tall, clear: 80, scale: 1, in: view)!))
  // Its Card out of view: the bird's mark at the edge says where it is.
  #expect(WritingPanel.frame(card: card.offsetBy(dx: 2000, dy: 0), clear: 80, scale: 1, in: view) == nil)
  // Readable at any zoom, and not larger than the view needs.
  #expect(WritingPanel.scale(zoom: 0.5) == 0.8 && WritingPanel.scale(zoom: 1) == 1 && WritingPanel.scale(zoom: 3) == 1.3)
}


@MainActor @Test func oneSaveAnimatesTogetherAndRunSupersedesIt() {
  let p = AgentPresence(animateWork: { true })
  let t = Date(timeIntervalSinceReferenceDate: 1000)
  let wires = (0..<12).map { AgentPresence.Wire(from: "a", fromPort: "out", to: "b\($0)", toPort: "in") }
  p.turnStarted(now: t)
  p.saved(written: [("a", reveal: true), ("b", reveal: false)], wires: wires, now: t)
  #expect(p.beats.count == 1)
  let save = p.beats[0]
  let mid = save.start.addingTimeInterval(save.length / 2)
  for w in wires {
    guard case .drawing(let progress, let sag) = p.thread(w, at: mid) else { Issue.record("wire must draw"); return }
    #expect(abs(progress - 0.5) < 0.0001 && sag == 0)
  }
  #expect(abs((p.writing("a", at: mid)?.reveal ?? -1) - 0.5) < 0.0001)
  #expect((p.writing("b", at: mid)?.settle ?? 0) > 0.99)
  for w in wires { #expect(p.thread(w, at: save.start.addingTimeInterval(save.length)) == .plain) }
  p.enqueue(.run("start"), now: mid)
  #expect(p.current(at: mid)?.beat.beat == .run("start"))
  #expect(p.line(at: mid)?.text.isEmpty == false)
  #expect(p.writing("a", at: mid) == nil)
  for w in wires { #expect(p.thread(w, at: mid) == .plain) }
  // Every later event replaces playback, including another save and visits.
  for i in 0..<3 {
    p.saved(written: [("c\(i)", reveal: false)], wires: wires, now: mid)
    p.enqueue(.visit("v\(i)"), now: mid)
    #expect(p.beats.count <= 2)
    #expect(p.current(at: mid)?.beat.beat == .visit("v\(i)"))
    #expect(p.beats.allSatisfy { $0.start <= mid })
  }
}

@MainActor @Test func newerActivityFinishesSavedEffectsForEveryHelper() {
  let t = Date(timeIntervalSinceReferenceDate: 1000)
  let birds = (0..<3).map { _ in AgentPresence(animateWork: { true }) }
  for p in birds {
    p.turnStarted(now: t)
    p.saved(written: [("a", reveal: true)], wires: [], now: t)
    p.doing("checking", now: t.addingTimeInterval(0.05))
    #expect(p.writing("a", at: t.addingTimeInterval(0.05)) == nil)
    #expect(p.line(at: t.addingTimeInterval(0.05))?.text == "checking")
  }
}

@MainActor @Test func presenceUsesTheLayoutCameraExactlyOnceEvenDuringFlight() {
  let t = Date(timeIntervalSinceReferenceDate: 1000)
  let p = AgentPresence(animateWork: { true })
  let cards = ["a": CGRect(x: 100, y: 150, width: 200, height: 100),
               "b": CGRect(x: 600, y: 350, width: 200, height: 100)]
  p.enqueue(.visit("a"), now: t)
  p.enqueue(.visit("b"), now: t.addingTimeInterval(1))
  for zoom in [0.25, 0.7, 1.0, 2.5] {
    let base = CanvasCamera(x: 40, y: 60, zoom: zoom)
    let geometry = PresenceGeometry(card: { cards[$0] }, wire: { _ in nil }, camera: base)
    let ch = Choreography(presence: p, g: geometry)
    for seconds in [1.0, 1.12, 1.3] {
      let pose = ch.pose(at: t.addingTimeInterval(seconds))!
      let before = geometry.screen(pose.center)
      var camera = base
      camera.x += 73; camera.y -= 39
      let host = CanvasPanHost<EmptyView>.Container()
      host.base = base; host.now = camera
      let d = host.shift
      let shown = CGPoint(x: before.x * d.scale + d.dx, y: before.y * d.scale + d.dy)
      #expect(abs(shown.x - before.x - 73) < 0.0001)
      #expect(abs(shown.y - before.y + 39) < 0.0001)
      #expect(abs(shown.x - camera.screen(pose.center).x) < 0.0001 && abs(shown.y - camera.screen(pose.center).y) < 0.0001)
      // Zoom/scroll, then a new layout pass (also used after a resize),
      // preserve the same world pose and agree with the Card transform.
      camera.zoom(to: zoom * 1.1, around: CGPoint(x: 320, y: 240))
      host.now = camera
      let shift = host.shift
      let zoomed = CGPoint(x: before.x * shift.scale + shift.dx, y: before.y * shift.scale + shift.dy)
      let expected = camera.screen(pose.center)
      #expect(abs(zoomed.x - expected.x) < 0.0001 && abs(zoomed.y - expected.y) < 0.0001)
      let relaid = PresenceGeometry(card: { cards[$0] }, wire: { _ in nil }, camera: camera)
      #expect(Choreography(presence: p, g: relaid).pose(at: t.addingTimeInterval(seconds))?.center == pose.center)
    }
  }
}

@MainActor @Test func anAcceptedRunStopsHelpersFlightsAndSavedEffectsAtOnce() {
  let t = Date(timeIntervalSinceReferenceDate: 1000)
  let p = AgentPresence(animateWork: { true })
  p.turnStarted(now: t)
  p.enqueue(.visit("a"), now: t)
  p.enqueue(.visit("b"), now: t.addingTimeInterval(1))
  let geometry = PresenceGeometry(card: { k in CGRect(x: k == "a" ? 0 : 500, y: 100, width: 200, height: 100) },
                                  wire: { _ in nil }, camera: CanvasCamera())
  let ch = Choreography(presence: p, g: geometry)
  let now = t.addingTimeInterval(1.1)
  #expect(ch.pose(at: now)?.mood == .flying)
  p.running(now: now)
  #expect(ch.pose(at: now)?.mood == .perched)
  #expect(ch.pose(at: now)?.center == ch.perch("b"))
  #expect(p.line(at: now)?.text.isEmpty == false)
}

@MainActor @Test func disablingSavedEffectsKeepsLatestWorkAndStreaming() {
  let p = AgentPresence(animateWork: { false })
  let t = Date(timeIntervalSinceReferenceDate: 1000)
  let w = AgentPresence.Wire(from: "a", fromPort: "out", to: "b", toPort: "in")
  p.saved(written: [("a", reveal: true)], wires: [w], now: t)
  #expect(p.thread(w, at: t) == .plain)
  #expect(p.writing("a", at: t) == nil)
  #expect(p.current(at: t)?.beat.beat.card == "a")
}

}
