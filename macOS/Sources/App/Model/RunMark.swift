import Foundation
import TsugumiRPC

enum RunMarkLabel {
  static var partial: String { String(localized: "部分実行") }
  static var derived: String { String(localized: "部分実行の結果を使用") }
}

/// Map the core's run provenance to a localized mark. Do not infer it from
/// propagation: downstream runs can use saved inputs too.
func runMark(_ run: Tsugumi_RPC_RunStatusT?) -> String? {
  guard let run else { return nil }
  let marks = [run.partialRun ? RunMarkLabel.partial : nil,
               run.usesPartialResult ? RunMarkLabel.derived : nil].compactMap { $0 }
  return marks.isEmpty ? nil : marks.joined(separator: " · ")
}
