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
      case .workflowSettings(let ref):
        if let s = AppModel.shared.sessions.values.first(where: { $0.identity.url.path == ref.path }) {
          ProjectSettingsPane.open(s, with: openSettings)
        }
      case .settings: openSettings()
      }
      UIScript.shared.request = nil
    }
    // The app's menu commands (⌘N, ⌘O, ⌘⇧H, ⌘⇧S).
    .onReceive(NotificationCenter.default.publisher(for: .appHome)) { _ in leaveProject() }
    .onReceive(NotificationCenter.default.publisher(for: .appNewProject)) { _ in
      if workspace.session != nil { leaveProject() }
      model.requestNewProject = true
    }
    .onReceive(NotificationCenter.default.publisher(for: .appOpenProject)) { _ in openFromPanel() }
    .onReceive(NotificationCenter.default.publisher(for: .appDuplicateProject)) { _ in duplicateProject() }
    .tsugumiLoading(model.preparing)
    // A window closed with a project: saved, then given back like leaving it.
    .onDisappear {
      guard let s = workspace.session else { return }
      Task {
        if s.dirty { await s.save() }
        if !s.dirty { await model.release(s) }
      }
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
      let s = try model.session(for: url)
      model.hold(s)
      workspace.open(s)
    } catch {
      model.error = "\(error)"
    }
  }

  private func openFromPanel() {
    let p = NSOpenPanel()
    p.canChooseDirectories = true
    p.canChooseFiles = false
    p.message = String(localized: "tsugumi.json を含むフォルダを選んでください")
    guard p.runModal() == .OK, let url = p.url else { return }
    if workspace.session != nil {
      leaveProject()
      Task {
        // Opens once the current project has been left (saved first when needed).
        for _ in 0..<50 where workspace.session != nil { try? await Task.sleep(for: .milliseconds(100)) }
        if workspace.session == nil { open(url) }
      }
    } else {
      open(url)
    }
  }

  /// A copy of the open project (its saved document and sources), then opened.
  private func duplicateProject() {
    guard let s = workspace.session else { return }
    let p = NSSavePanel()
    p.nameFieldStringValue = s.identity.url.lastPathComponent + " copy"
    p.message = String(localized: "複製を作る場所を選んでください（保存済みの内容を複製します）")
    guard p.runModal() == .OK, let url = p.url else { return }
    Task {
      do {
        try await s.duplicate(to: url)
        openFromCopy(url)
      } catch {
        switchError = String(localized: "複製できませんでした: ") + "\(error)"
      }
    }
  }

  private func openFromCopy(_ url: URL) {
    leaveProject()
    Task {
      for _ in 0..<50 where workspace.session != nil { try? await Task.sleep(for: .milliseconds(100)) }
      if workspace.session == nil { open(url) }
    }
  }

  /// Back to the chooser: allowed only when nothing runs and the needed save succeeded.
  private func leaveProject() {
    guard let s = workspace.session else { return }
    if s.runActive {
      switchError = String(localized: "実行中はプロジェクトを切り替えられません。停止するか終了を待ってください。")
      return
    }
    Task {
      if s.dirty { await s.save() }
      if s.dirty {
        switchError = String(localized: "変更を保存できませんでした: \(s.saveError ?? "")。変更は保持しています。")
        return
      }
      workspace.close()
      await model.release(s)
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

/// Shown when this Mac cannot run the shared VM yet: virtualization or the
/// VM image is missing. Everything else is set up on the first project start.
struct EnvironmentSetupView: View {
  @Environment(AppModel.self) private var model

  var body: some View {
    let s = model.setup ?? EnvironmentManager.SetupStatus()
    VStack(alignment: .leading, spacing: 18) {
      Label("Tsugumi の実行環境を準備します", systemImage: "shippingbox").font(.largeTitle.bold())
      Text("Card は Tsugumi の Linux VM（Virtualization.framework、アプリ全体で 1 台）の中で、プロジェクトごとに隔離して実行されます。")
        .foregroundStyle(.secondary)
      GroupBox {
        VStack(alignment: .leading, spacing: 12) {
          step(String(localized: "この Mac で仮想マシンを使える"), ok: s.virtualizationSupported,
               detail: String(localized: "Apple silicon の Mac と macOS 26 以降が必要です。")) { EmptyView() }
          step(String(localized: "実行環境の VM イメージがある"), ok: s.imageAvailable,
               detail: s.imageAvailable ? String(localized: "VM イメージ \(s.imageVersion ?? "")")
                                        : String(localized: "アプリに VM イメージが含まれていません。開発中は macOS/Container/build-image.sh --vm で作成してください。")) {
            EmptyView()
          }
        }
        .padding(8)
      }
      Text("初めてプロジェクトを開いたときに、VM イメージの展開と Nix store の保存領域（最大 \(model.environments.resources.storeGiB) GiB、使った分だけ）を作ります。")
        .font(.callout).foregroundStyle(.secondary)
      HStack {
        Spacer()
        Button("再確認") { Task { await model.refreshSetup() } }.disabled(model.checkingSetup)
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

}
