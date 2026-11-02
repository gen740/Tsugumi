import AppKit
import SwiftUI

@main
struct TsugumiApp: App {
  @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
  private let model = AppModel.shared
  private let prefs = AppPreferences.shared

  init() {
    // A project folder given on the command line is opened by the app itself
    // (AppModel.takeLaunchProject). AppKit must not take it as a file to open:
    // then SwiftUI would not create the main window at all.
    UserDefaults.standard.register(defaults: ["NSTreatUnknownArgumentsAsOpen": "NO"])
  }

  var body: some Scene {
    WindowGroup("Tsugumi", id: "main") {
      MainWindow()
        .environment(model)
        .environment(prefs)
        .frame(minWidth: 1100, minHeight: 680)
    }
    .commands { WorkbenchCommands() }

    // A child workflow opened in its own window.
    WindowGroup("Workflow", id: "workflow", for: WorkflowRef.self) { $ref in
      if let ref { WorkflowWindow(ref: ref).environment(model).environment(prefs).frame(minWidth: 900, minHeight: 600) }
    }

    WindowGroup("Card 設定", id: "card", for: CardRef.self) { $ref in
      // One editor per Card: a window shown for another Card never keeps the
      // previous Card's working copy.
      if let ref { DetailHost(path: ref.path) { CardSettingsEditor(session: $0, ref: ref).id(ref.card) } }
    }
    .defaultSize(width: 860, height: 620)
    .windowToolbarStyle(.unifiedCompact(showsTitle: true))

    WindowGroup("Draft", id: "draft", for: CardRef.self) { $ref in
      if let ref { DetailHost(path: ref.path) { DraftEditor(session: $0, cardKey: ref.card) } }
    }
    .defaultSize(width: 640, height: 560)

    WindowGroup("Card 定義", id: "definitions", for: ProjectRef.self) { $ref in
      if let ref { DetailHost(path: ref.path) { CardDatabaseView(session: $0, initial: ref.definition) } }
    }
    .defaultSize(width: 900, height: 600)

    WindowGroup("実行履歴", id: "executions", for: CardRef.self) { $ref in
      if let ref { DetailHost(path: ref.path) { CardExecutionView(session: $0, cardKey: ref.card) } }
    }
    .defaultSize(width: 900, height: 560)

    WindowGroup("成果物", id: "artifacts", for: CardRef.self) { $ref in
      if let ref { DetailHost(path: ref.path) { CardArtifactsView(session: $0, cardKey: ref.card) } }
    }
    .defaultSize(width: 900, height: 560)

    WindowGroup("Workflow 設定", id: "workflow-settings", for: ProjectRef.self) { $ref in
      if let ref { DetailHost(path: ref.path) { WorkflowSettingsView(session: $0) } }
    }
    .defaultSize(width: 680, height: 480)
    .windowToolbarStyle(.unifiedCompact(showsTitle: true))

    WindowGroup("可視化", id: "visual", for: CardRef.self) { $ref in
      if let ref { DetailHost(path: ref.path) { VisualizationWindow(session: $0, cardKey: ref.card) } }
    }
    .defaultSize(width: 900, height: 600)

    WindowGroup("チャット", id: "chat", for: ProjectRef.self) { $ref in
      if let ref {
        DetailHost(path: ref.path) { s in ChatPanel(session: s, chat: AppModel.shared.chat(for: s), detached: true) }
      }
    }
    .defaultSize(width: 460, height: 720)

    Settings {
      ApplicationSettings()
        .environment(prefs)
        .frame(width: 760, height: 520)
    }
  }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
  func applicationDidFinishLaunching(_ notification: Notification) {
    // Launched with `swift run` there is no bundle: still behave as a regular app.
    NSApp.setActivationPolicy(.regular)
    NSApp.activate(ignoringOtherApps: true)
    Task { @MainActor in
      AppPreferences.shared.applyTheme()
      SmokeTest.runIfRequested(AppModel.shared)
      UIScript.runIfRequested()
    }
  }
}

/// Resolves the project of a detail window and keeps it in the environment.
struct DetailHost<Content: View>: View {
  let path: String
  @ViewBuilder let content: (ProjectSession) -> Content
  @State private var session: ProjectSession?
  @State private var error: String?

  var body: some View {
    Group {
      if let session {
        content(session)
          .environment(AppModel.shared)
          .environment(AppPreferences.shared)
          .focusedSceneValue(\.projectSession, session)
          .navigationSubtitle(session.scopeName([]))
      } else if let error {
        ContentUnavailableView("プロジェクトを開けません", systemImage: "exclamationmark.triangle", description: Text(error))
      } else {
        ProgressView("読み込み中…")
      }
    }
    .task {
      do { session = try AppModel.shared.session(for: URL(fileURLWithPath: path)) } catch { self.error = "\(error)" }
    }
  }
}

/// Menu commands routed to the focused window's project and tab.
struct WorkbenchCommands: Commands {
  @FocusedValue(\.projectSession) private var session
  @FocusedValue(\.workspace) private var workspace
  @FocusedValue(\.workflowTab) private var tab

  var body: some Commands {
    CommandGroup(replacing: .undoRedo) {
      Button("取り消す") { session?.undo() }
        .keyboardShortcut("z")
        .disabled(session?.undoStack.isEmpty ?? true)
      Button("やり直す") { session?.redo() }
        .keyboardShortcut("z", modifiers: [.command, .shift])
        .disabled(session?.redoStack.isEmpty ?? true)
    }
    CommandGroup(after: .saveItem) {
      Button("保存") { if let s = session { Task { await s.save() } } }
        .shortcut(.save)
        .disabled(!(session?.dirty ?? false))
    }
    CommandMenu("Workflow") {
      Button("実行") { if let s = session, let t = tab { RunActions.runAll(s, scope: t.scope) } }
        .shortcut(.run)
        .disabled(!(session?.connected ?? false) || (session?.runActive ?? true))
      Button("停止") { if let s = session { Task { await s.cancelRun() } } }
        .shortcut(.stop)
        .disabled(!(session?.runActive ?? false))
      Divider()
      Button("全体表示") { NotificationCenter.default.post(name: .canvasFitAll, object: tab?.id) }
        .shortcut(.fitAll)
      Button("整列") { if let s = session, let t = tab { s.arrange(t.scope) } }
        .shortcut(.arrange)
        .disabled(!(session?.connected ?? false))
      Button("付箋を追加") { NotificationCenter.default.post(name: .canvasAddNote, object: tab?.id) }
        .shortcut(.addNote)
      Button("複製") {
        if let s = session, let t = tab, s.isEditable { t.selection = s.duplicate(t.selection, in: t.scope) }
      }
      .shortcut(.duplicate)
      .disabled(tab?.selection.isEmpty ?? true)
      Divider()
      Button("チャットの表示切替") { workspace?.showChat.toggle() }
        .shortcut(.toggleChat)
      Button("ログの表示切替") { workspace?.togglePanel(.log) }
        .shortcut(.toggleLog)
      Button("タイムラインの表示切替") { workspace?.togglePanel(.timeline) }
        .shortcut(.toggleTimeline)
    }
  }
}

extension Notification.Name {
  static let canvasFitAll = Notification.Name("tsugumi.canvas.fitAll")
  static let canvasAddNote = Notification.Name("tsugumi.canvas.addNote")
  /// Moves a canvas (object: tab id) to a Card (userInfo "card").
  static let canvasReveal = Notification.Name("tsugumi.canvas.reveal")
  /// Shows the details bubble of a Card (object: tab id, userInfo "card").
  static let canvasShowDetails = Notification.Name("tsugumi.canvas.showDetails")
}

struct ProjectSessionKey: FocusedValueKey { typealias Value = ProjectSession }
struct WorkspaceKey: FocusedValueKey { typealias Value = Workspace }
struct WorkflowTabKey: FocusedValueKey { typealias Value = WorkflowTab }

extension FocusedValues {
  var projectSession: ProjectSession? {
    get { self[ProjectSessionKey.self] }
    set { self[ProjectSessionKey.self] = newValue }
  }
  var workspace: Workspace? {
    get { self[WorkspaceKey.self] }
    set { self[WorkspaceKey.self] = newValue }
  }
  var workflowTab: WorkflowTab? {
    get { self[WorkflowTabKey.self] }
    set { self[WorkflowTabKey.self] = newValue }
  }
}
