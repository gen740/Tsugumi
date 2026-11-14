import AppKit
import SwiftUI
import TsugumiLauncher

/// A main window: initial setup when the host needs it, then the project
/// chooser, then the workbench tabs of the project it shows. There can be
/// several, each with its own project (MainWindowRouting.swift).
struct MainWindow: View {
  @Environment(AppModel.self) private var model
  @State private var workspace = Workspace()

  @Environment(\.openWindow) private var openWindow
  @Environment(\.openSettings) private var openSettings

  var body: some View {
    Group {
      if model.setupNeeded {
        EnvironmentSetupView()
      } else if let s = workspace.session {
        WorkflowTabs(workspace: workspace, session: s, onSwitchProject: { model.leaveProject(in: workspace) })
      } else {
        ProjectChooser(workspace: workspace, onOpen: { model.openProject($0, from: workspace, openWindow: openWindow) })
      }
    }
    .onChange(of: UIScript.shared.request) { _, r in
      // Scripted UI checks (TSUGUMI_UI_SCRIPT) open windows the way menus do.
      guard let r, UIScript.shared.workspace === workspace else { return }
      switch r {
      case .card(let ref): openWindow(id: "card", value: ref)
      case .visual(let ref): openWindow(id: "visual", value: ref)
      case .definitions(let ref): openWindow(id: "definitions", value: ref)
      case .versions(let ref): openWindow(id: "versions", value: ref)
      case .workflowSettings(let ref):
        if let s = AppModel.shared.sessions.values.first(where: { $0.identity.url.path == ref.path }) {
          ProjectSettingsPane.open(s, with: openSettings)
        }
      case .settings: openSettings()
      case .explainer: openWindow(id: "explainer")
      case .project(let url): model.openProject(url, from: workspace, openWindow: openWindow)
      case .newProject: model.newProject(from: workspace, openWindow: openWindow)
      }
      UIScript.shared.request = nil
    }
    // The menu's commands for the window in front (WorkbenchCommands).
    .focusedSceneValue(\.mainWindow, workspace)
    // Closing it while a run goes would stop the run: like leaving the project, not until it ended.
    .windowDismissBehavior(workspace.session?.runActive == true ? .disabled : .automatic)
    .tsugumiLoading(model.preparing)
    // Tsugumi flies over the whole window (from its corner or the chat).
    .modifier(GuideWindowLayer(project: workspace.session?.identity.url.path))
    // The guide's buttons act in this window.
    .environment(\.guideWindow, guideActions)
    .background(WindowReader { workspace.window = $0 })
    .onReceive(NotificationCenter.default.publisher(for: NSWindow.willCloseNotification)) { n in
      guard let closing = n.object as? NSWindow, closing === workspace.window else { return }
      model.windowClosed(workspace)
    }
    .task {
      model.register(workspace)
      guard workspace.session == nil else { return }
      switch model.takeWindowIntent() {
      case .open(let url)?: model.show(url, in: workspace)
      case .newProject?: workspace.requestNewProject = true
      case nil:
        // `Tsugumi <project folder>` opens that project in the first window.
        if let url = model.takeLaunchProject() { model.show(url, in: workspace) }
      }
    }
    .task { if model.setup == nil { await model.refreshSetup() } }
    .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
      Task { await CorePreferences.shared.reload() }
    }
    // Right after the user's first key (or sign-in): Tsugumi asks once, in one window, whether to show the demo.
    .sheet(isPresented: Binding(get: { GuideController.shared.demoInvite == workspace.id },
                                set: { if !$0, GuideController.shared.demoInvite == workspace.id { GuideController.shared.demoInvite = nil } })) {
      GuideInviteView(see: {
        GuideController.shared.demoInvite = nil
        GuidePerformer(model: model, window: guideActions, session: workspace.session, tab: workspace.active,
                       openWindow: openWindow, openSettings: openSettings)(GuideAction(kind: .openDemo, label: "", demo: "report"))
      }, later: {
        GuideController.shared.demoInvite = nil
        GuideController.shared.answerDemoOffer()
      })
    }
    .alert("この操作はできません", isPresented: Binding(get: { workspace.alert != nil }, set: { if !$0 { workspace.alert = nil } })) {
      Button("OK") { workspace.alert = nil }
    } message: { Text(workspace.alert ?? "") }
    .alert("保存できない変更があります", isPresented: Binding(get: { workspace.unsavedReason != nil }, set: { if !$0 { workspace.unsavedReason = nil } })) {
      Button("変更を破棄して Home へ戻る", role: .destructive) { model.discardAndLeave(in: workspace) }
      Button("キャンセル", role: .cancel) {}
    } message: {
      Text("Home へ戻るには、保存できない変更を破棄します。破棄した変更は取り消せません。\n\n保存できない理由: \(workspace.unsavedReason ?? "")")
    }
  }

  /// What the guide's buttons do in this window.
  private var guideActions: GuideWindowActions {
    GuideWindowActions(
      openProject: { model.openProject($0, from: workspace, openWindow: openWindow) },
      home: { model.leaveProject(in: workspace) },
      newProject: { model.newProject(from: workspace, openWindow: openWindow) },
      showChat: { workspace.showChat = true },
      showPanel: { p in workspace.bottomPanel = p; workspace.showBottom = true })
  }
}

/// A child workflow in a window of its own (same document and run as the main window).
struct WorkflowWindow: View {
  @Environment(AppModel.self) private var model
  let ref: WorkflowRef
  @State private var workspace = Workspace()
  @State private var error: String?
  @State private var didRevealSelection = false

  var body: some View {
    Group {
      if let s = workspace.session {
        WorkflowTabs(workspace: workspace, session: s, onSwitchProject: nil)
          .onChange(of: s.rev) { _, _ in
            if !didRevealSelection, let selected = ref.selected, let tab = workspace.active, s.instance(selected) != nil {
              didRevealSelection = true
              DispatchQueue.main.async { NotificationCenter.default.post(name: .canvasReveal, object: tab.id, userInfo: ["card": selected]) }
            }
          }
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
        workspace.tabs.first?.selection = ref.selected.map { [$0] } ?? []
        if let selected = ref.selected, let tab = workspace.tabs.first, s.instance(selected) != nil {
          didRevealSelection = true
          DispatchQueue.main.async { NotificationCenter.default.post(name: .canvasReveal, object: tab.id, userInfo: ["card": selected]) }
        }
        workspace.showChat = false
      } catch { self.error = "\(error)" }
    }
    .modifier(ClosesWithProject(session: workspace.session))
  }
}

/// Shows a failed host check separately from confirmed missing prerequisites.
/// Everything else is set up on the first project start.
struct EnvironmentSetupView: View {
  @Environment(AppModel.self) private var model

  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      Label("Tsugumi の実行環境を準備します", systemImage: "shippingbox").font(.largeTitle.bold())
      Text("Card は Tsugumi の Linux VM（Virtualization.framework、アプリ全体で 1 台）の中で、プロジェクトごとに隔離して実行されます。")
        .foregroundStyle(.secondary)
      GroupBox {
        VStack(alignment: .leading, spacing: 12) {
          switch model.setup {
          case .failed(let error)?:
            Label("実行環境の状態を確認できませんでした", systemImage: "exclamationmark.triangle")
              .font(.headline)
            Text(error).font(.callout).textSelection(.enabled)
          case .checked(let supported, let image, let version)?:
            step(String(localized: "この Mac で仮想マシンを使える"), ok: supported,
                 detail: String(localized: "Apple silicon の Mac と macOS 26 以降が必要です。")) { EmptyView() }
            step(String(localized: "実行環境の VM イメージがある"), ok: image,
                 detail: image ? String(localized: "VM イメージ \(version ?? "")")
                               : String(localized: "アプリに VM イメージが含まれていません。開発中は macOS/Container/build-image.sh --vm で作成してください。")) {
              EmptyView()
            }
          case nil:
            ProgressView("実行環境の状態を確認しています…")
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
