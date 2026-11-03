import Foundation

/// Why starting, using or removing an execution environment failed.
public enum LauncherError: Error, CustomStringConvertible, Sendable {
  case imageMissing
  case startFailed(reason: String, log: String)
  case notReady(String)
  case locked
  case invalidProject(String)
  case deleteFailed(what: String, log: String)

  /// Shown in the app: Japanese source strings, English from the app's
  /// string table (macOS/Resources/tools/en_strings.py).
  public var description: String {
    switch self {
    case .imageMissing:
      return String(localized: "Tsugumi の実行環境のイメージが見つかりません。", bundle: .main)
    case .startFailed(let reason, _):
      return String(localized: "実行環境が起動の途中で止まりました: \(Self.reasonText(reason))", bundle: .main)
    case .notReady(let s):
      return String(localized: "実行環境の準備ができませんでした: \(s)", bundle: .main)
    case .locked:
      return String(localized: "別の Tsugumi の処理がこの実行環境を起動または停止しています。", bundle: .main)
    case .invalidProject(let s):
      return String(localized: "Tsugumi のプロジェクトではありません: \(s)", bundle: .main)
    case .deleteFailed(let what, let log):
      return String(localized: "\(what) を削除できませんでした: \(log.trimmingCharacters(in: .whitespacesAndNewlines))", bundle: .main)
    }
  }

  private static func reasonText(_ r: String) -> String {
    switch r {
    case "tsugumid exited during startup": return String(localized: "このプロジェクトの tsugumid が起動中に終了しました", bundle: .main)
    case "no readiness signal within 90 s": return String(localized: "90 秒たっても準備ができませんでした", bundle: .main)
    default: return r
    }
  }

  /// What the user can do about it, shown with the error.
  public var recovery: String {
    switch self {
    case .imageMissing:
      return String(localized: "macOS/Container/build-image.sh で作り直すか、アプリを入れ直してください。", bundle: .main)
    case .startFailed:
      return String(localized: "下の原因とログを確かめてください。直したら再試行します。文書と実行履歴は消えません。", bundle: .main)
    case .notReady:
      return String(localized: "実行環境を停止してから再試行してください。続くときは実行環境のデータを削除します（文書は消えません）。", bundle: .main)
    case .locked:
      return String(localized: "もう一方の処理が終わるのを待ってから、再試行してください。", bundle: .main)
    case .invalidProject:
      return String(localized: "tsugumi.json のあるフォルダを選んでください。", bundle: .main)
    case .deleteFailed:
      return String(localized: "これより後の削除はしていません。実行環境のファイルは残っています。もう一度削除してください。", bundle: .main)
    }
  }
}
