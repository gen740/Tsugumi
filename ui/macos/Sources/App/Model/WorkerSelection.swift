import Foundation
import TsugumiRPC

/// What a failure says to a person. A Card that never started because no
/// worker could be chosen is said in plain words: the core names each
/// worker's reason in the failure's `worker:<id>` arguments (and a timed-out
/// probe's limit in `timeout:<id>`); its diagnostic stays the full text.
extension Tsugumi_Data_FailureT {
  var plainText: String? {
    guard let reasons = workerSelectionReasons else { return diagnostic }
    let head = String(localized: "worker を選べなかったため、この Card は開始していません")
    return reasons.isEmpty ? head : head + ": " + reasons.joined(separator: String(localized: "、"))
  }

  /// The reasons in a line (for the Card's hover), when the Card never started
  /// because no worker could be chosen; nil for any other failure.
  var notStartedText: String? {
    workerSelectionReasons.map { $0.joined(separator: String(localized: "、")) }
  }

  /// Each worker's reason, when no worker could be chosen.
  private var workerSelectionReasons: [String]? {
    let args = self.args.compactMap { $0 }
    guard args.contains(where: { $0.key == "stage" && $0.value == "worker_selection" }) else { return nil }
    return args.compactMap { a -> String? in
      guard a.key.hasPrefix("worker:") else { return nil }
      let id = String(a.key.dropFirst("worker:".count))
      let name = WorkerProbeText.workerName(id)
      switch a.value {
      case "timeout":
        if let limit = args.first(where: { $0.key == "timeout:" + id })?.value {
          return String(localized: "\(name) の probe が時間切れ（\(limit) 秒）")
        }
        return String(localized: "\(name) の probe が時間切れ")
      case "failed": return String(localized: "\(name) の probe が失敗")
      case "not_positive": return String(localized: "\(name) の probe のスコアが 0 以下")
      default: return String(localized: "\(name) は使えません")
      }
    }
  }
}

enum WorkerProbeText {
  static func workerName(_ id: String) -> String {
    id == "local" ? String(localized: "プロジェクトの実行環境") : id
  }

  /// One worker's measurement, as its row in a run's Info says it.
  static func result(_ probe: Tsugumi_Data_WorkerProbeResultT) -> String {
    if let score = probe.score {
      return probe.probe == nil ? String(localized: "計測なし（同順位）") : "\(String(localized: "スコア")) \(score.formatted())"
    }
    if let reason = probe.unavailableReason { return reason }
    if probe.failureKind == .timeout {
      let head = probe.timeLimitSeconds.map { String(localized: "時間切れ（\(String($0)) 秒）") } ?? String(localized: "時間切れ")
      return probe.failure.map { head + ": " + $0 } ?? head
    }
    return probe.failure ?? String(localized: "計測に失敗しました")
  }
}
