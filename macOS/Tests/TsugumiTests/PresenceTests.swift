import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiRPC

@Test func toolCallsTellWhichCardTheAgentLooksAt() {
  #expect(PresenceEvents.look(tool: "project_file_read", arguments: #"{"path": "definitions/bands/src/main.py"}"#) == .visit(["bands"]))
  #expect(PresenceEvents.look(tool: "run", arguments: #"{"card": ["start"], "propagate": true}"#) == .run(["start"]))
  #expect(PresenceEvents.look(tool: "run_record", arguments: #"{"run_id": "r", "card": ["rgb"]}"#) == .visit(["rgb"]))
  // Not a Card's file, nor a tool that looks at one.
  #expect(PresenceEvents.look(tool: "project_file_read", arguments: #"{"path": "tsugumi.json"}"#) == nil)
  #expect(PresenceEvents.look(tool: "reference_read", arguments: #"{"path": "card-guide.md"}"#) == nil)
  #expect(PresenceEvents.look(tool: "run", arguments: "not json") == nil)
}

@Test func aSaveWritesOutDraftsUpstreamFirstAndThreadsTheirWires() {
  typealias C = PresenceEvents.Connection
  let wiring = [C(from: "start", fromPort: "out", to: "load", toPort: "start"),
                C(from: "load", fromPort: "events", to: "bands", toPort: "input")]
  let x = ["start": 0.0, "load": 200, "bands": 500]
  let w = PresenceEvents.written(
    upserted: ["bands", "load"], touchedSources: ["load"], existedBefore: ["start", "load", "bands"],
    templateBefore: ["start": "trigger", "load": "draft", "bands": "draft"],
    templateAfter: ["start": "trigger", "bands": "draft"],
    cards: ["start", "load", "bands"], connectionsBefore: wiring, connectionsAfter: wiring, order: { x[$0] ?? 0 })
  // load: a Draft that became code (written out); bands: still a Draft (only touched).
  #expect(w.cards.map(\.0) == ["load", "bands"])
  #expect(w.cards.map(\.reveal) == [true, false])
  // The wire out of the Card written out is threaded again; others stay.
  #expect(w.wires == [C(from: "load", fromPort: "events", to: "bands", toPort: "input")])

  // A wire the save adds is threaded too; Cards in Card and unknown ids are left out.
  let added = C(from: "bands", fromPort: "images", to: "rgb", toPort: "input")
  let w2 = PresenceEvents.written(
    upserted: ["rgb", "inner"], touchedSources: [], existedBefore: ["rgb"], templateBefore: [:], templateAfter: [:],
    cards: ["bands", "rgb"], connectionsBefore: wiring, connectionsAfter: wiring + [added], order: { _ in 0 })
  #expect(w2.cards.map(\.0) == ["rgb"] && w2.cards.map(\.reveal) == [false])
  #expect(w2.wires == [added])
}

@MainActor @Test func scenesPlayOneAfterAnotherWithoutLaggingFarBehind() {
  let p = AgentPresence()
  let t0 = Date(timeIntervalSinceReferenceDate: 1000)
  p.turnStarted(now: t0)
  p.enqueue(.visit("load"), now: t0)
  p.saved(written: [("load", reveal: true)], wires: [AgentPresence.Wire(from: "load", fromPort: "events", to: "bands", toPort: "input")], now: t0)
  #expect(p.beats.count == 3)
  // Each starts where the one before ends.
  #expect(p.beats[1].start == p.beats[0].end && p.beats[2].start == p.beats[1].end)
  // Where the bird already is (the thread brought it to bands), and a visit
  // twice in a row, are not shown again.
  p.enqueue(.visit("bands"), now: t0)
  #expect(!p.beats.contains { $0.beat == .visit("bands") })
  p.enqueue(.visit("rgb"), now: t0)
  p.enqueue(.visit("rgb"), now: t0)
  #expect(p.beats.filter { $0.beat == .visit("rgb") }.count == 1)
  // Visits that would start far behind the Agent are dropped.
  for i in 0..<10 { p.enqueue(.write("c\(i)", reveal: false), now: t0) }
  p.enqueue(.visit("late"), now: t0)
  #expect(!p.beats.contains { $0.beat == .visit("late") })
  // Visible during the turn and a while after its scenes.
  #expect(p.visible(at: t0))
  p.turnEnded(now: t0)
  let last = p.beats.last!.end
  #expect(p.visible(at: last.addingTimeInterval(1)))
  #expect(!p.visible(at: last.addingTimeInterval(AgentPresence.Timing.linger + 1)))
}

@MainActor @Test func aWrittenCardRevealsThenItsColourSettlesAndFades() {
  let p = AgentPresence()
  let t0 = Date(timeIntervalSinceReferenceDate: 1000)
  p.enqueue(.visit("start"), now: t0)
  p.enqueue(.write("load", reveal: true), now: t0)
  let s = p.beats[1].start
  // Queued: the new face waits hidden behind the Draft.
  #expect(p.writing("load", at: t0)?.reveal == 0)
  // Written out left to right, then only the colour remains, then nothing.
  let mid = p.writing("load", at: s.addingTimeInterval(AgentPresence.Timing.revealStart + AgentPresence.Timing.revealLength / 2))
  #expect(mid?.reveal.map { $0 > 0.3 && $0 < 0.7 } == true)
  let after = p.writing("load", at: s.addingTimeInterval(2.6))
  #expect(after?.reveal == nil && (after?.settle ?? 0) > 0.5)
  #expect(p.writing("load", at: s.addingTimeInterval(10)) == nil)
  #expect(p.writing("other", at: s) == nil)
}

@MainActor @Test func aThreadHangsWhileDrawnAndIsPulledTaut() {
  let p = AgentPresence()
  let t0 = Date(timeIntervalSinceReferenceDate: 1000)
  let w = AgentPresence.Wire(from: "a", fromPort: "out", to: "b", toPort: "in")
  p.enqueue(.thread(w), now: t0)
  let T = AgentPresence.Timing.self
  #expect(p.thread(w, at: t0) == .hidden)
  guard case .drawing(let half, let sag) = p.thread(w, at: t0.addingTimeInterval(T.threadStart + T.threadLength / 2)) else {
    Issue.record("drawing expected"); return
  }
  #expect(half > 0.3 && half < 0.7 && sag == 1)
  guard case .drawing(let full, let taut) = p.thread(w, at: t0.addingTimeInterval(T.threadStart + T.threadLength * 1.4)) else {
    Issue.record("still settling expected"); return
  }
  #expect(full == 1 && abs(taut) < 0.6)
  #expect(p.thread(w, at: t0.addingTimeInterval(T.threadStart + T.threadLength * 2)) == .plain)
  #expect(p.thread(AgentPresence.Wire(from: "x", fromPort: "o", to: "y", toPort: "i"), at: t0) == .plain)
}

@Test func thoughtLinesAreOneShortLine() {
  #expect(ThoughtSummarizer.clean("「帯域の境界を広げる」\n二行目") == "帯域の境界を広げる")
  #expect(ThoughtSummarizer.clean("  \n") == nil)
  #expect(ThoughtSummarizer.clean(String(repeating: "あ", count: 100))?.count == 60)
}

@MainActor @Test func theBirdFadesInAndAwayInsteadOfPopping() {
  let p = AgentPresence()
  let t0 = Date(timeIntervalSinceReferenceDate: 1000)
  let T = AgentPresence.Timing.self
  p.turnStarted(now: t0)
  p.enqueue(.visit("load"), now: t0)
  #expect(p.fade(at: t0).opacity == 0)
  #expect(p.fade(at: t0.addingTimeInterval(T.fadeIn)).opacity == 1)
  p.turnEnded(now: t0.addingTimeInterval(1))
  let end = p.beats.last!.end
  // Still fully there, the done line said and then gone before the bird.
  #expect(p.fade(at: end.addingTimeInterval(1)).opacity == 1)
  #expect(p.line(at: end.addingTimeInterval(1))?.text == "できました")
  #expect(p.line(at: end.addingTimeInterval(T.doneLine + 0.1)) == nil)
  // Fading and rising over the last moment of its stay.
  let mid = p.fade(at: end.addingTimeInterval(T.linger - T.fadeOut / 2))
  #expect(mid.opacity > 0.2 && mid.opacity < 0.8 && mid.lift > 0.2)
  #expect(p.fade(at: end.addingTimeInterval(T.linger)).opacity == 0)
}
