import AppKit
import SwiftUI
import TsugumiLauncher

struct OpenVMStartupLogButton: View {
  @Environment(\.openSettings) private var openSettings

  var body: some View {
    Button {
      UserDefaults.standard.set(ApplicationSettings.Category.executionEnvironment.rawValue, forKey: "settings.category")
      openSettings()
    } label: {
      Label("起動ログで詳細を見る", systemImage: "apple.terminal")
    }
  }
}

/// The shared VM's console: how the execution environment was set up and
/// started (image, store disk, boot, Nix, network, supervisor) and what the
/// supervisor did since. Read-only; commands go to a project's terminal.
struct VMStartupLog: View {
  @Environment(AppModel.self) private var model
  @State private var lines: [String] = []
  @State private var state: VMState = .stopped
  @State private var tokens: (UUID, UUID)?
  @State private var follow = true

  private var vm: HostRuntimeSnapshot { model.environments.vm }

  var body: some View {
    VStack(spacing: 0) {
      HStack(spacing: 10) {
        Label("共有 VM の起動ログ", systemImage: "apple.terminal").font(.headline)
        Text(stateText).font(.callout).foregroundStyle(.secondary)
        Spacer()
        Toggle("末尾を追う", isOn: $follow).toggleStyle(.checkbox).controlSize(.small)
        Button("コピー") {
          NSPasteboard.general.clearContents()
          NSPasteboard.general.setString(lines.joined(separator: "\n"), forType: .string)
        }
        .controlSize(.small)
        Button("ログのフォルダ") { NSWorkspace.shared.activateFileViewerSelecting([vm.directory.appendingPathComponent("console.log")]) }
          .controlSize(.small)
          .help("console.log（ゲスト）、console.prev.log（前回の起動）、launcher.log（アプリの記録）")
      }
      .padding(8)
      Divider()
      ScrollViewReader { proxy in
        ScrollView {
          LazyVStack(alignment: .leading, spacing: 0) {
            ForEach(Array(lines.enumerated()), id: \.offset) { i, l in
              Text(verbatim: l.isEmpty ? " " : l)
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(color(l))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .id(i)
            }
          }
          .padding(8)
        }
        .overlay {
          if lines.isEmpty {
            ContentUnavailableView("まだ出力はありません", systemImage: "apple.terminal",
                                   description: Text("プロジェクトを開くと実行環境が起動し、その記録がここに表示されます。"))
          }
        }
        .onChange(of: lines.count) { _, n in if follow, n > 0 { proxy.scrollTo(n - 1, anchor: .bottom) } }
      }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .onAppear {
      guard tokens == nil else { return }
      lines = vm.console.snapshot
      let a = vm.console.observe { l in Task { @MainActor in lines.append(l); if lines.count > 5000 { lines.removeFirst(lines.count - 5000) } } }
      let b = vm.observe { s in Task { @MainActor in state = s } }
      tokens = (a, b)
    }
    .onDisappear {
      if let (a, b) = tokens {
        vm.console.stopObserving(a)
        vm.stopObserving(b)
        tokens = nil
      }
    }
  }

  private var stateText: String {
    switch state {
    case .stopped: return String(localized: "停止中")
    case .starting(let s): return String(localized: "起動中: \(EnvironmentOverlay.stageText(EnvironmentStage(vmStage: s).rawValue))")
    case .running: return String(localized: "稼働中")
    case .failed(let m): return String(localized: "失敗: \(m)")
    }
  }

  private func color(_ l: String) -> SwiftUI.Color {
    if l.hasPrefix("TSUGUMI-FAILED") || l.localizedCaseInsensitiveContains("error") || l.contains("failed") { return .red }
    if l.hasPrefix("TSUGUMI-STAGE") { return .accentColor }
    if l.hasPrefix("[tsugumi]") { return .secondary }
    return .primary
  }
}
