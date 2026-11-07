import AppKit
import Foundation
import SwiftUI
import Testing
@testable import TsugumiApp
import TsugumiLauncher
import TsugumiRPC

/// The guide's guide_show: what it points at and the buttons it offers.
@Test func guideShowIsReadIntoButtons() {
  let shown = GuideAction.parse(#"{"highlight": " run ", "actions": [{"kind": "run", "label": "実行する"},"#
    + #"{"kind": "open_demo", "demo": "live-stream", "label": ""}, {"kind": "save", "label": "x"},"#
    + #"{"kind": "open_card", "card": "summarize", "label": "設定"}, {"kind": "home", "label": "Home"}]}"#)
  #expect(shown.highlight == "run")
  // Unknown kinds are dropped; at most three buttons; an empty label falls back to the kind.
  #expect(shown.actions.map(\.kind) == [.run, .openDemo, .openCard])
  #expect(shown.actions[1].demo == "live-stream" && shown.actions[1].label == "open_demo")
  #expect(shown.actions[2].card == "summarize")
  #expect(GuideAction.parse("{").actions.isEmpty && GuideAction.parse("{}").highlight == nil)
  // What the bird says and the replies to tap (empty ones dropped, at most four).
  let spoken = GuideAction.parse(#"{"highlight": "card:generate", "say": " CSV を作ります。 ", "choices": ["次へ", " ", "もっと詳しく", "a", "b", "c"]}"#)
  #expect(spoken.say == "CSV を作ります。" && spoken.choices == ["次へ", "もっと詳しく", "a", "b"])
}

/// The bubble says the opening of an answer that gave it no line: whole
/// sentences up to about 120 characters.
@Test func theBubbleSaysTheOpening() {
  #expect(GuideController.firstSentence("**実行は成功**しました。全 Card が動きました。") == "実行は成功しました。全 Card が動きました。")
  let long = "これは最初の文です。" + String(repeating: "い", count: 115) + "。"
  #expect(GuideController.firstSentence(long) == "これは最初の文です。")
  #expect(GuideController.firstSentence("\n## 見方\n- `heading` が見出しです") == "見方")
  #expect(GuideController.firstSentence("   ") == nil)
  #expect((GuideController.firstSentence(String(repeating: "あ", count: 200)) ?? "").count == 139)
  #expect(GuideAction.parse(#"{"endTour": true}"#).endTour && !GuideAction.parse("{}").endTour)
  #expect(GuideAction.parse(#"{"open": " card:archive/copy ", "highlight": "param:rows"}"#).open == "card:archive/copy")
}

/// A long answer sends the user to the chat; one the bubble holds does not.
@Test func aLongAnswerPointsToTheChat() {
  #expect(GuideController.saysMore(String(repeating: "あ", count: 300), than: "要点です。"))
  #expect(!GuideController.saysMore("要点です。少しだけ補足。", than: "要点です。"))
}

/// On the ChatGPT plan Tsugumi speaks with gpt-6.1-luna if the account offers it, else its newest luna.
@Test func theSpeechModelIsOneThePlanOffers() {
  #expect(AgentSettings.speechModel(offered: [], fallback: "x") == "x")
  #expect(AgentSettings.speechModel(offered: ["gpt-6.1-sol", "gpt-6.1-luna"], fallback: "x") == "gpt-6.1-luna")
  #expect(AgentSettings.speechModel(offered: ["gpt-6.1-sol", "gpt-6-luna", "gpt-5.6-luna"], fallback: "x") == "gpt-6-luna")
  #expect(AgentSettings.speechModel(offered: ["gpt-6.1-sol"], fallback: "gpt-6.1-sol") == "gpt-6.1-sol")
}

/// "card:<id>" also points at that Card inside the Card in Card on the screen.
@MainActor @Test func aCardIsFoundInsideACardInCard() {
  let g = GuideController.shared
  #expect(!g.points(at: "card:archive/copy"))
}

/// The tour's copy is recognised whichever spelling of its folder a window has.
@Test func theTourFolderIsTheSameThroughLinks() throws {
  let name = "tsugumi-guide-\(UUID().uuidString)"
  try FileManager.default.createDirectory(atPath: "/private/tmp/\(name)", withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(atPath: "/private/tmp/\(name)") }
  #expect(GuideController.same("/tmp/\(name)/", URL(fileURLWithPath: "/private/tmp/\(name)")))
  #expect(!GuideController.same("/tmp/\(name)-copy", URL(fileURLWithPath: "/private/tmp/\(name)")))
}

/// By a Card the bird goes with the Canvas, within limits; what is not on a
/// Canvas has no zoom and leaves it as it is. The web has the same limits
/// (web/tests/perchScale.tsx).
@MainActor @Test func theBirdByACardGoesWithTheCanvas() {
  let full = GuideFlyer.birdScale
  #expect(GuideFlyer.perchScale(zoom: nil) == full)
  #expect(GuideFlyer.perchScale(zoom: 1) == full)
  #expect(GuideFlyer.perchScale(zoom: 2) == full)
  #expect(GuideFlyer.perchScale(zoom: 0.7) == full * 0.7)
  #expect(GuideFlyer.perchScale(zoom: 0.45) == full * 0.45)
  #expect(GuideFlyer.perchScale(zoom: 0.39) == full * 0.45)
  // The frame the bird flies in, at 39%: 90 x 64 is 40.5 x 28.8.
  #expect(abs(900 * GuideFlyer.perchScale(zoom: 0.39) - 40.5) < 0.001)
}

/// A Card on the real canvas tells the stage the canvas's zoom: without it the
/// bird would stand at its full size whatever the zoom.
@MainActor @Test func aCardOnTheCanvasTellsTheBirdItsZoom() async throws {
  let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("guide-zoom-\(UUID().uuidString.prefix(8))")
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: dir) }
  let document = #"{"name": "t", "canvas": {"cards": [], "connections": []}}"#
  try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  s.project = try JSONValue.parse(document)
  let card = s.addCard(template: .trigger, in: [], at: CGPoint(x: 100, y: 100), name: "start")
  s.cameras[key([])] = (x: 0, y: 0, zoom: 0.39)

  let guide = GuideController.shared
  guide.point(at: "card:\(card)")
  defer { guide.clearHighlight() }
  let stage = GuideStage()
  let view = NSHostingView(rootView: GraphCanvas(workspace: Workspace(), session: s, tab: WorkflowTab(scope: [])).environment(stage).environment(AppPreferences.shared))
  let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 600), styleMask: [.borderless], backing: .buffered, defer: true)
  window.contentView = view
  view.layoutSubtreeIfNeeded()
  for _ in 0..<100 where stage.perch == nil {
    try await Task.sleep(for: .milliseconds(20))
    view.layoutSubtreeIfNeeded()
  }
  #expect(stage.perch?.id == "card:\(card)")
  #expect(stage.perch?.spot.zoom == 0.39)
  window.contentView = nil
}
