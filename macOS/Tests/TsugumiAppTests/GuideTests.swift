import Foundation
import Testing
@testable import TsugumiApp

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
