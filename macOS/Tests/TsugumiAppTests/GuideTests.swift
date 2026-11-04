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
}

/// The tour's copy is recognised whichever spelling of its folder a window has.
@Test func theTourFolderIsTheSameThroughLinks() throws {
  let name = "tsugumi-guide-\(UUID().uuidString)"
  try FileManager.default.createDirectory(atPath: "/private/tmp/\(name)", withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(atPath: "/private/tmp/\(name)") }
  #expect(GuideController.same("/tmp/\(name)/", URL(fileURLWithPath: "/private/tmp/\(name)")))
  #expect(!GuideController.same("/tmp/\(name)-copy", URL(fileURLWithPath: "/private/tmp/\(name)")))
}
