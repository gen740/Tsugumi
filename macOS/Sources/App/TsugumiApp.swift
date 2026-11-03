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

    Window("実行環境のコンソール", id: "console") {
      VMConsoleView().environment(model).environment(prefs)
    }
    .defaultSize(width: 760, height: 460)
    WindowGroup("チャット", id: "chat", for: ProjectRef.self) { $ref in
      if let ref {
        DetailHost(path: ref.path) { s in ChatPanel(session: s, chat: AppModel.shared.chat(for: s), detached: true) }
      }
    }
    .defaultSize(width: 460, height: 720)

    Settings {
      // The connection and storage panes read the open projects from the model.
      ApplicationSettings()
        .environment(model)
        .environment(prefs)
        .frame(width: 760, height: 520)
    }
  }
}

/// Window menu: the shared VM's console.
struct OpenConsoleButton: View {
  @Environment(\.openWindow) private var openWindow
  var body: some View {
    Button("実行環境のコンソール") { openWindow(id: "console") }
      .keyboardShortcut("k", modifiers: [.command, .option])
  }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
  private var shuttingDown = false

  /// Quitting stops every project (runs are recorded as interrupted) and
  /// powers the shared VM off before the app exits.
  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    guard !shuttingDown else { return .terminateLater }
    guard case .stopped = AppModel.shared.environments.vm.state else {
      shuttingDown = true
      // Off the main actor, and the reply through the run loop: terminate
      // may have been called from a main-actor task, which keeps the main
      // queue busy while AppKit waits in its own loop for the reply.
      let environments = AppModel.shared.environments
      Task.detached {
        await environments.shutdown()
        CFRunLoopPerformBlock(CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue) {
          sender.reply(toApplicationShouldTerminate: true)
        }
        CFRunLoopWakeUp(CFRunLoopGetMain())
      }
      return .terminateLater
    }
    return .terminateNow
  }

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

/// Menu commands routed to the focused window's project and tab. Single-key
/// shortcuts (S, A, L, …) are enabled only while the canvas has the focus,
/// so typing in a text field never triggers them.
struct WorkbenchCommands: Commands {
  @FocusedValue(\.projectSession) private var session
  @FocusedValue(\.workspace) private var workspace
  @FocusedValue(\.workflowTab) private var tab
  @FocusedValue(\.canvasFocused) private var canvasFocused
  @Environment(\.openWindow) private var openWindow

  private var onCanvas: Bool { canvasFocused ?? false }
  private var selected: CardKey? { tab?.selection.sorted().first }
  private func post(_ n: Notification.Name, _ info: [String: Any]? = nil) {
    NotificationCenter.default.post(name: n, object: n.rawValue.hasPrefix("tsugumi.app.") ? nil : tab?.id, userInfo: info)
  }

  var body: some Commands {
    CommandGroup(replacing: .newItem) {
      Button("新しいプロジェクト…") { post(.appNewProject) }.shortcut(.newProject)
      Button("プロジェクトを開く…") { post(.appOpenProject) }.shortcut(.openProject)
      Button("Home へ戻る") { post(.appHome) }.shortcut(.home).disabled(session == nil)
    }
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
      Button("プロジェクトを複製…") { post(.appDuplicateProject) }
        .shortcut(.duplicateProject)
        .disabled(session == nil)
    }
    // ⌘F searches what is shown: the file in a code editor, Card names on the canvas.
    CommandGroup(replacing: .textEditing) {
      Button("テキスト検索…") { if !TextFind.showInFocusedEditor() { post(.canvasFind) } }
        .shortcut(.find)
    }
    CommandMenu("Workflow") {
      Button("ワークフローを実行") { if let s = session, let t = tab { RunActions.runAll(s, scope: t.scope) } }
        .shortcut(.run)
        .disabled(!(session?.connected ?? false) || (session?.runActive ?? true))
      Button("実行を停止") { if let s = session { Task { await s.cancelRun() } } }
        .shortcut(.stop)
        .disabled(!(session?.runActive ?? false))
      Divider()
      Section("Canvas（フォーカスがあるとき）") {
        Button("Card 設定を開く") {
          if let s = session, let k = selected { openWindow(id: "card", value: CardRef(path: s.identity.url.path, card: k)) }
        }
        .shortcut(.openSettings).disabled(!onCanvas || selected == nil)
        Button("成果物を開く") {
          if let s = session, let k = selected { openWindow(id: "artifacts", value: CardRef(path: s.identity.url.path, card: k)) }
        }
        .shortcut(.openArtifacts).disabled(!onCanvas || selected == nil)
        Button("実行ログを開く") { if let k = selected { workspace?.showLog(for: k) } }
          .shortcut(.openLog).disabled(!onCanvas || selected == nil)
        Button("折りたたみを切り替える") {
          if let s = session, let t = tab { s.setCollapsed(t.selection, !t.selection.allSatisfy { s.collapsed($0) }) }
        }
        .shortcut(.toggleCollapse).disabled(!onCanvas || (tab?.selection.isEmpty ?? true))
        Button("Card を複製") {
          if let s = session, let t = tab, s.isEditable { Task { t.selection = await s.duplicate(t.selection, in: t.scope) } }
        }
        .shortcut(.duplicate).disabled(!onCanvas || (tab?.selection.isEmpty ?? true))
        Button("選択対象を削除") { post(.canvasDelete) }
          .keyboardShortcut(.delete, modifiers: [])
          .disabled(!onCanvas || ((tab?.selection.isEmpty ?? true) && tab?.selectedConnection == nil))
        Divider()
        Button("全体表示") { post(.canvasFitAll) }.shortcut(.fitAll).disabled(!onCanvas)
        Button("100% で表示") { post(.canvasZoom, ["zoom": 0.0]) }.shortcut(.actualSize).disabled(!onCanvas)
        Button("拡大") { post(.canvasZoom, ["zoom": 1.2]) }.shortcut(.zoomIn).disabled(!onCanvas)
        Button("縮小") { post(.canvasZoom, ["zoom": 1 / 1.2]) }.shortcut(.zoomOut).disabled(!onCanvas)
        Button("整列") { if let s = session, let t = tab { s.arrange(t.scope) } }
          .shortcut(.arrange).disabled(!onCanvas || !(session?.connected ?? false))
        Button("グループ操作") { post(.canvasGroup) }
          .shortcut(.group).disabled(!onCanvas || (tab?.selection.isEmpty ?? true))
        Divider()
        Button("Draft Card を追加") { post(.canvasAddDraft) }
          .shortcut(.addDraft).disabled(!onCanvas || !(session?.isEditable ?? false))
        Button("Draft から Card を生成") {
          guard let s = session, let t = tab else { return }
          let drafts = s.cards(in: t.scope).compactMap { $0["id"]?.string }.map { key(t.scope + [$0]) }.filter { s.kind($0) == .draft }
          let chosen = drafts.filter { t.selection.contains($0) }
          workspace?.showChat = true
          AppModel.shared.chat(for: s).generateDrafts(scope: t.scope, cards: chosen.isEmpty ? drafts : chosen)
        }
        .shortcut(.generateDrafts).disabled(!onCanvas || (session?.dirty ?? true) || (session?.runActive ?? true))
      }
      Divider()
      Button("付箋を追加") { post(.canvasAddNote) }
        .shortcut(.addNote)
      Button("チャットの表示切替") { workspace?.showChat.toggle() }
        .shortcut(.toggleChat)
      Button("ログの表示切替") { workspace?.togglePanel(.log) }
        .shortcut(.toggleLog)
      Button("タイムラインの表示切替") { workspace?.togglePanel(.timeline) }
        .shortcut(.toggleTimeline)
      Button("ターミナルの表示切替") { workspace?.togglePanel(.terminal) }
        .shortcut(.toggleTerminal)
    }
    CommandGroup(after: .windowList) {
      OpenConsoleButton()
    }
  }
}

/// ⌘F in a code editor: its own find bar (the canvas searches Card names).
@MainActor
enum TextFind {
  static func showInFocusedEditor() -> Bool {
    guard let tv = NSApp.keyWindow?.firstResponder as? NSTextView, tv.usesFindBar else { return false }
    let item = NSMenuItem()
    item.tag = NSTextFinder.Action.showFindInterface.rawValue
    tv.performTextFinderAction(item)
    return true
  }
}

extension Notification.Name {
  /// Home and project commands of the menu (handled by the main window).
  static let appNewProject = Notification.Name("tsugumi.app.newProject")
  static let appOpenProject = Notification.Name("tsugumi.app.openProject")
  static let appHome = Notification.Name("tsugumi.app.home")
  static let appDuplicateProject = Notification.Name("tsugumi.app.duplicateProject")
  /// Canvas commands (object: tab id).
  static let canvasDelete = Notification.Name("tsugumi.canvas.delete")
  static let canvasGroup = Notification.Name("tsugumi.canvas.group")
  static let canvasAddDraft = Notification.Name("tsugumi.canvas.addDraft")
  static let canvasFitAll = Notification.Name("tsugumi.canvas.fitAll")
  static let canvasAddNote = Notification.Name("tsugumi.canvas.addNote")
  /// Moves a canvas (object: tab id) to a Card (userInfo "card").
  static let canvasReveal = Notification.Name("tsugumi.canvas.reveal")
  /// A Card was renamed (userInfo: from, to, project): views showing it follow.
  static let cardRenamed = Notification.Name("tsugumi.card.renamed")
  /// Zooms a canvas (object: tab id): userInfo "zoom" factor, 0 = 100 %.
  static let canvasZoom = Notification.Name("tsugumi.canvas.zoom")
  /// Opens the Card search of a canvas (object: tab id).
  static let canvasFind = Notification.Name("tsugumi.canvas.find")
  /// Shows the details bubble of a Card (object: tab id, userInfo "card").
  static let canvasShowDetails = Notification.Name("tsugumi.canvas.showDetails")
}

struct ProjectSessionKey: FocusedValueKey { typealias Value = ProjectSession }
struct WorkspaceKey: FocusedValueKey { typealias Value = Workspace }
struct WorkflowTabKey: FocusedValueKey { typealias Value = WorkflowTab }
struct CanvasFocusedKey: FocusedValueKey { typealias Value = Bool }

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
  /// The canvas has the keyboard focus (not a text field).
  var canvasFocused: Bool? {
    get { self[CanvasFocusedKey.self] }
    set { self[CanvasFocusedKey.self] = newValue }
  }
}
