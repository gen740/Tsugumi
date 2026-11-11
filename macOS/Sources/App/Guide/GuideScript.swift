import Foundation
import TsugumiAgentClient
import TsugumiRPC

/// What Tsugumi says without a model connection (agent/Shared/guide-script.json;
/// the web reads the same file): the tours of a few demos, each step a
/// guide_show with the step every choice leads to, what Home's first question
/// is answered with, and one reply to anything typed. Nothing is sent anywhere.
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

  /// Steps by "<demo>/<id>" ("noConnection" and "otherProject" have no demo).
  let steps: [String: Step]
  /// The steps answered on Home (each tour's "home").
  let homeSteps: Set<String>
  /// The demos with a tour, in order, and the words that pick each on Home.
  let tours: [(demo: String, keywords: [String])]
  var noConnection: Step { steps["noConnection"]! }

  /// In the app's language.
  static let shared = GuideScript(json: AppPreferences.Language.running == .en ? Agent.guideScriptEnglish : Agent.guideScript)

  init(json: String) {
    let v = (try? JSONValue.parse(json)) ?? .object([])
    func step(_ s: JSONValue?, in demo: String? = nil) -> Step {
      // A step of the same tour, unless it names its tour.
      func resolve(_ id: String?) -> String? { id.map { $0.contains("/") || demo == nil ? $0 : "\(demo!)/\($0)" } }
      let show = GuideAction.parse(s?["show"]?.serialized(pretty: false) ?? "{}")
      let next = Dictionary((s?["next"]?.object ?? []).compactMap { k, v in resolve(v.string).map { (k, $0) } }) { a, _ in a }
      return Step(show: show, text: s?["text"]?.string ?? show.say ?? "", next: next,
                  ranOk: resolve(s?["ran"]?["ok"]?.string), ranElse: resolve(s?["ran"]?["else"]?.string))
    }
    var steps: [String: Step] = [:]
    var homeSteps: Set<String> = []
    var tours: [(String, [String])] = []
    for (demo, t) in v["tours"]?.object ?? [] {
      tours.append((demo, (t["keywords"]?.array ?? []).compactMap(\.string)))
      for (id, s) in t["home"]?.object ?? [] {
        steps["\(demo)/\(id)"] = step(s, in: demo)
        homeSteps.insert("\(demo)/\(id)")
      }
      for (id, s) in t["steps"]?.object ?? [] { steps["\(demo)/\(id)"] = step(s, in: demo) }
    }
    steps["noConnection"] = step(v["noConnection"])
    steps["otherProject"] = step(v["otherProject"])
    self.steps = steps
    self.homeSteps = homeSteps
    self.tours = tours
  }

  /// The demo Home's first question picks for an answer: the first with one of its words, else the first.
  func demo(for interest: String) -> String {
    tours.first { $0.keywords.contains { interest.localizedCaseInsensitiveContains($0) } }?.demo ?? tours.first?.demo ?? "report"
  }

  /// Where a demo's tour begins (or continues): "otherProject" for a demo without one.
  func step(_ id: String, of demo: String) -> String {
    steps["\(demo)/\(id)"] != nil ? "\(demo)/\(id)" : "otherProject"
  }
}
