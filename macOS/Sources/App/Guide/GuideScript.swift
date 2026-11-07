import Foundation
import TsugumiAgent
import TsugumiRPC

/// What Tsugumi says without a model connection (agent/Shared/guide-script.json;
/// the web reads the same file): the steps of the tour of the demo report, each a
/// guide_show with the step every choice leads to, and one reply to anything
/// typed. Nothing is sent anywhere.
struct GuideScript {
  struct Step {
    var show: GuideShow
    /// What the panel or the chat shows (the bubble's line when the step has no text of its own).
    var text: String
    /// The step a tapped choice leads to.
    var next: [String: String]
    /// While this step is out, the step a run's end leads to (succeeded, or otherwise).
    var ranOk: String?
    var ranElse: String?
  }

  let steps: [String: Step]
  let noConnection: Step

  static let shared = GuideScript(json: Agent.guideScript)

  init(json: String) {
    let v = (try? JSONValue.parse(json)) ?? .object([])
    func step(_ s: JSONValue?) -> Step {
      let show = GuideAction.parse(s?["show"]?.serialized(pretty: false) ?? "{}")
      let next = Dictionary((s?["next"]?.object ?? []).compactMap { k, v in v.string.map { (k, $0) } }) { a, _ in a }
      return Step(show: show, text: s?["text"]?.string ?? show.say ?? "", next: next,
                  ranOk: s?["ran"]?["ok"]?.string, ranElse: s?["ran"]?["else"]?.string)
    }
    steps = Dictionary((v["steps"]?.object ?? []).map { ($0.0, step($0.1)) }) { a, _ in a }
    noConnection = step(v["noConnection"])
  }
}
