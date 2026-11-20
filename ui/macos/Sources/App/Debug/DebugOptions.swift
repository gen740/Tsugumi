import SwiftUI

/// What the debug panel turns on: presentation only, kept in UserDefaults and
/// off by default. None of it changes a project or what a run does.
@MainActor @Observable
final class DebugOptions {
  static let shared = DebugOptions()
  @ObservationIgnored private let d = UserDefaults.standard

  /// The canvas outlines what takes a click: each Card's hit shape, the band
  /// around each wire that selects it, and the canvas's solid rectangles.
  var showHitAreas: Bool { didSet { d.set(showHitAreas, forKey: "debug.showHitAreas") } }
  /// A failed Agent tool call shows its arguments and the whole result as
  /// returned, not only the short reason.
  var fullErrorDetail: Bool { didSet { d.set(fullErrorDetail, forKey: "debug.fullErrorDetail") } }
  /// The details over a Card or a wire add their keys (Card key, connection
  /// index and ports), as they appear in tsugumi.json and the logs.
  var showKeys: Bool { didSet { d.set(showKeys, forKey: "debug.showKeys") } }
  /// A corner of the canvas shows the camera, what is on it and how long the
  /// last wire routing took.
  var showCanvasStats: Bool { didSet { d.set(showCanvasStats, forKey: "debug.showCanvasStats") } }

  private init() {
    showHitAreas = d.bool(forKey: "debug.showHitAreas")
    fullErrorDetail = d.bool(forKey: "debug.fullErrorDetail")
    showKeys = d.bool(forKey: "debug.showKeys")
    showCanvasStats = d.bool(forKey: "debug.showCanvasStats")
  }

  var anyOn: Bool { showHitAreas || fullErrorDetail || showKeys || showCanvasStats }

  func reset() {
    showHitAreas = false
    fullErrorDetail = false
    showKeys = false
    showCanvasStats = false
  }
}

/// The debug panel in the Window menu.
struct DebugPanel: View {
  @Bindable private var options = DebugOptions.shared

  var body: some View {
    TabView {
      presentation.tabItem { Text("表示オプション") }
      if ProcessInfo.processInfo.environment["TSUGUMI_RPC_TRACE"] == "1" {
        RPCTraceView().tabItem { Text("RPC トレース") }
      }
    }
  }

  private var presentation: some View {
    Form {
      Section("Canvas") {
        Toggle("クリックの当たり判定を表示", isOn: $options.showHitAreas)
        Text("Card の当たり範囲（青）と、配線を選べる帯（橙）を Canvas に重ねて描きます。")
          .font(.caption).foregroundStyle(.secondary)
        Toggle("Card と配線のキーを詳細に表示", isOn: $options.showKeys)
        Text("ポインタを置いたときの詳細に、tsugumi.json やログでの Card キーと接続の番号を加えます。")
          .font(.caption).foregroundStyle(.secondary)
        Toggle("Canvas の状態を表示", isOn: $options.showCanvasStats)
        Text("カメラ、Card と配線の数、最後の配線ルーティングにかかった時間を Canvas の隅に出します。")
          .font(.caption).foregroundStyle(.secondary)
      }
      Section("チャット") {
        Toggle("ツールのエラーを詳しく表示", isOn: $options.fullErrorDetail)
        Text("失敗したツール呼び出しに、短い理由に加えて引数と返ってきた結果の全文を出します。")
          .font(.caption).foregroundStyle(.secondary)
      }
      Section {
        Button("すべてオフにする") { options.reset() }.disabled(!options.anyOn)
      }
    }
    .formStyle(.grouped)
    .frame(minWidth: 420, minHeight: 360)
  }
}

/// Window menu: the debug panel.
struct OpenDebugPanelButton: View {
  @Environment(\.openWindow) private var openWindow
  var body: some View {
    Button("デバッグパネル") { openWindow(id: "debug") }
  }
}
