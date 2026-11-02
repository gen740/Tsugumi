import AppKit
import SwiftUI
import TsugumiLauncher

/// The main window: initial setup when the host needs it, then the project
/// chooser, then the workbench tabs of the opened project.
struct MainWindow: View {
  @Environment(AppModel.self) private var model
  @State private var workspace = Workspace()
  @State private var switchError: String?

  @Environment(\.openWindow) private var openWindow
  @Environment(\.openSettings) private var openSettings

  var body: some View {
    Group {
      if model.setupNeeded {
        EnvironmentSetupView()
      } else if let s = workspace.session {
        WorkflowTabs(workspace: workspace, session: s, onSwitchProject: leaveProject)
      } else {
        ProjectChooser(onOpen: open)
      }
    }
    .onChange(of: UIScript.shared.request) { _, r in
      // Scripted UI checks (TSUGUMI_UI_SCRIPT) open windows the way menus do.
      guard let r else { return }
      switch r {
      case .card(let ref): openWindow(id: "card", value: ref)
      case .draft(let ref): openWindow(id: "draft", value: ref)
      case .definitions(let ref): openWindow(id: "definitions", value: ref)
      case .workflowSettings(let ref): openWindow(id: "workflow-settings", value: ref)
      case .settings: openSettings()
      }
      UIScript.shared.request = nil
    }
    .task {
      UIScript.shared.workspace = workspace
      if model.setup == nil { await model.refreshSetup() }
      // `Tsugumi <project folder>` opens that project in the first window.
      if workspace.session == nil, let url = model.takeLaunchProject() { open(url) }
    }
    .alert("プロジェクトを切り替えられません", isPresented: Binding(get: { switchError != nil }, set: { if !$0 { switchError = nil } })) {
      Button("OK") { switchError = nil }
    } message: { Text(switchError ?? "") }
  }

  private func open(_ url: URL) {
    do {
      workspace.open(try model.session(for: url))
    } catch {
      model.error = "\(error)"
    }
  }

  /// Back to the chooser: allowed only when nothing runs and the needed save succeeded.
  private func leaveProject() {
    guard let s = workspace.session else { return }
    if s.runActive {
      switchError = "実行中はプロジェクトを切り替えられません。停止するか終了を待ってください。"
      return
    }
    Task {
      if s.dirty { await s.save() }
      if s.dirty {
        switchError = "変更を保存できませんでした: \(s.saveError ?? "")。変更は保持しています。"
        return
      }
      workspace.close()
    }
  }
}

/// A child workflow in a window of its own (same document and run as the main window).
struct WorkflowWindow: View {
  @Environment(AppModel.self) private var model
  let ref: WorkflowRef
  @State private var workspace = Workspace()
  @State private var error: String?

  var body: some View {
    Group {
      if let s = workspace.session {
        WorkflowTabs(workspace: workspace, session: s, onSwitchProject: nil)
      } else if let error {
        ContentUnavailableView("開けません", systemImage: "exclamationmark.triangle", description: Text(error))
      } else {
        ProgressView()
      }
    }
    .task {
      do {
        let s = try model.session(for: URL(fileURLWithPath: ref.path))
        workspace.open(s)
        workspace.tabs.first?.scope = ref.scope
        workspace.tabs.first?.selection = []
        workspace.showChat = false
      } catch { self.error = "\(error)" }
    }
  }
}

/// Shown when the container runtime or the environment image still needs setting up.
struct EnvironmentSetupView: View {
  @Environment(AppModel.self) private var model
  @State private var working: String?
  @State private var message: String?

  var body: some View {
    let s = model.setup ?? EnvironmentManager.SetupStatus()
    VStack(alignment: .leading, spacing: 18) {
      Label("Tsugumi の実行環境を準備します", systemImage: "shippingbox").font(.largeTitle.bold())
      Text("Card は隔離された Linux 環境（Apple container）の中で実行されます。次の項目を確認してください。")
        .foregroundStyle(.secondary)
      GroupBox {
        VStack(alignment: .leading, spacing: 12) {
          step("Apple container \(ContainerCLI.pinnedVersion) がインストールされている", ok: s.containerInstalled && s.versionSupported,
               detail: s.containerInstalled
                 ? (s.versionSupported ? "version \(s.version ?? "")" : "インストール済み: \(s.version ?? "不明")（検証済みは \(ContainerCLI.pinnedVersion)）")
                 : "github.com/apple/container/releases からインストールしてください。") {
            EmptyView()
          }
          step("container サービスが起動している", ok: s.serviceRunning, detail: "container system start を実行します。") {
            Button("サービスを起動") { act("サービスを起動しています…") { try await model.environments.startService() } }
              .disabled(!s.containerInstalled || s.serviceRunning || working != nil)
          }
          step("実行環境イメージが読み込まれている", ok: s.imageLoaded,
               detail: s.bundledImageAvailable ? "アプリに同梱のイメージを読み込みます（初回のみ、数分かかります）。"
                                               : "同梱イメージがありません。macOS/Container/build-image.sh で作成してください。") {
            Button("イメージを読み込む") { act("イメージを読み込んでいます…") { try await model.environments.loadBundledImage() } }
              .disabled(!s.serviceRunning || s.imageLoaded || !s.bundledImageAvailable || working != nil)
          }
        }
        .padding(8)
      }
      HStack {
        if let working { ProgressView().controlSize(.small); Text(working) }
        if let message { Label(message, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.red).textSelection(.enabled) }
        Spacer()
        Button("再確認") { Task { await model.refreshSetup() } }.disabled(working != nil || model.checkingSetup)
      }
    }
    .padding(32)
    .frame(maxWidth: 720)
    .frame(maxWidth: .infinity, maxHeight: .infinity)
  }

  private func step<A: View>(_ title: String, ok: Bool, detail: String, @ViewBuilder action: () -> A) -> some View {
    HStack(alignment: .top, spacing: 10) {
      Image(systemName: ok ? "checkmark.circle.fill" : "circle.dashed")
        .foregroundStyle(ok ? .green : .secondary)
        .font(.title3)
        .accessibilityLabel(ok ? "完了" : "未完了")
      VStack(alignment: .leading, spacing: 2) {
        Text(title).bold()
        Text(detail).font(.callout).foregroundStyle(.secondary)
      }
      Spacer()
      if !ok { action() }
    }
  }

  private func act(_ label: String, _ op: @escaping () async throws -> Void) {
    working = label
    message = nil
    Task {
      do { try await op() } catch { message = "\(error)" }
      working = nil
      await model.refreshSetup()
    }
  }
}
