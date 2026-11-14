import Foundation
import TsugumiRPC

enum RunMarkLabel {
  static var partial: String { String(localized: "部分実行") }
  static var derived: String { String(localized: "部分実行の結果を使用") }
}

/// Map the core's run provenance to a localized mark. Do not infer it from
/// propagation: downstream runs can use saved inputs too.
func runMark(_ run: Tsugumi_RPC_RunStatusT?) -> String? {
  // Wire the explicit provenance fields here when the protocol exposes them.
  return nil
}
