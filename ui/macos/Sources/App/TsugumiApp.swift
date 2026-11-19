import AppKit
import SwiftUI
import TsugumiLauncher

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
    // No window is restored (the scenes below), but a session that ended without
    // quitting (killed, crashed) leaves saved state, and a launch that has some to
    // restore makes no first window: SwiftUI leaves that to the restoration.
    UserDefaults.standard.register(defaults: ["ApplePersistenceIgnoreState": "YES"])
    // Sockets to the VM and to tsugumid: a peer that went away is an error
    // to handle, never the end of the app (sockets also set SO_NOSIGPIPE).
    signal(SIGPIPE, SIG_IGN)
    // Each project has a window of its own; windows do not merge into tabs.
    NSWindow.allowsAutomaticWindowTabbing = false
  }

  var body: some Scene {
    WindowGroup("Tsugumi", id: "main") {
      MainWindow()
        .task { await PermissionCatalog.shared.load() }
        .environment(model)
        .environment(prefs)
        .frame(minWidth: 1100, minHeight: 680)
    }
    .commands { WorkbenchCommands() }
    // No window comes back by itself: a main window would come back as an empty
    // Home, and a Card's settings without its project's window (the launch would then
    // show no main window at all). Every launch starts at one Home.
    .restorationBehavior(.disabled)

    // A child workflow opened in its own window.
    WindowGroup("Workflow", id: "workflow", for: WorkflowRef.self) { $ref in
      if let ref { WorkflowWindow(ref: ref).environment(model).environment(prefs).frame(minWidth: 900, minHeight: 600) }
    }
    .restorationBehavior(.disabled)

    WindowGroup("Card 設定", id: "card", for: CardRef.self) { $ref in
      // One editor per Card: a window shown for another Card never keeps the
      // previous Card's working copy.
      // Tsugumi can fly into it to point at a setting.
      if let ref { DetailHost(path: ref.path) { CardSettingsEditor(session: $0, ref: ref).id(ref.card) }.modifier(GuideWindowLayer(project: ref.path)) }
    }
    .defaultSize(width: 860, height: 620)
    .restorationBehavior(.disabled)
    .windowToolbarStyle(.unifiedCompact(showsTitle: true))

    WindowGroup("Card 定義", id: "definitions", for: ProjectRef.self) { $ref in
      if let ref { DetailHost(path: ref.path) { CardDatabaseView(session: $0, initial: ref.definition) } }
    }
    .defaultSize(width: 900, height: 600)
    .restorationBehavior(.disabled)

    WindowGroup("版の履歴", id: "versions", for: ProjectRef.self) { $ref in
      if let ref { DetailHost(path: ref.path) { VersionTreeView(session: $0) } }
    }
    .defaultSize(width: 980, height: 620)
    .restorationBehavior(.disabled)

    WindowGroup("実行履歴", id: "executions", for: CardRef.self) { $ref in
      if let ref { DetailHost(path: ref.path) { CardSettingsEditor(session: $0, ref: CardRef(path: ref.path, card: ref.card, category: .executions)) } }
    }
    .defaultSize(width: 900, height: 560)
    .restorationBehavior(.disabled)

    WindowGroup("成果物", id: "artifacts", for: CardRef.self) { $ref in
      if let ref { DetailHost(path: ref.path) { CardArtifactsView(session: $0, cardKey: ref.card) } }
    }
    .defaultSize(width: 900, height: 560)
    .restorationBehavior(.disabled)

    WindowGroup("可視化", id: "visual", for: CardRef.self) { $ref in
      if let ref { DetailHost(path: ref.path) { VisualizationWindow(session: $0, cardKey: ref.card) } }
    }
    .defaultSize(width: 900, height: 600)
    .restorationBehavior(.disabled)

    Window("実行環境のコンソール", id: "console") {
      VMConsoleView().environment(model).environment(prefs)
    }
    .defaultSize(width: 760, height: 460)
    .restorationBehavior(.disabled)
    Window("デバッグ", id: "debug") { DebugPanel() }
      .defaultSize(width: 460, height: 420)
      .windowResizability(.contentSize)
      .restorationBehavior(.disabled)
    WindowGroup("チャット", id: "chat", for: ProjectRef.self) { $ref in
      if let ref {
        DetailHost(path: ref.path) { s in ChatPanel(session: s, chat: AppModel.shared.chat(for: s), detached: true) }
      }
    }
    .defaultSize(width: 460, height: 720)
    .restorationBehavior(.disabled)

    Settings {
      // The connection and storage panes read the open projects from the model.
      ApplicationSettings()
        .environment(model)
        .environment(prefs)
        .frame(width: 760, height: 520)
    }
    .restorationBehavior(.disabled)
  }
}

/// Application menu: check for an update, or restart into the one downloaded.
struct UpdateMenuItem: View {
  private let updater = Updater.shared
  var body: some View {
    if case .ready(let v) = updater.state {
      Button("再起動して \(v) に更新") { updater.installAndRelaunch() }
    } else {
      if updater.busy {
        Button("キャンセル") { updater.cancelTransfer() }
      } else {
        Button("アップデートを確認…") { updater.check(manual: true) }
      }
    }
  }
}

/// Help menu: the documents, read with their figures.
struct OpenExplainerButton: View {
  var body: some View {
    Button("Tsugumi を知る") { Explainer.open() }
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

/// Quits the app from code. Quit waits in a nested run loop for work on the
/// main queue, and that queue is not drained from inside one of its own blocks
/// (a main-actor task, a main-queue source): the app would hang half quit. So
/// the request is handed to the run loop instead of calling terminate in place.
func requestQuit(_ before: @escaping @MainActor () -> Void = {}) {
  CFRunLoopPerformBlock(CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue) {
    MainActor.assumeIsolated {
      before()
      NSApp.terminate(nil)
    }
  }
  CFRunLoopWakeUp(CFRunLoopGetMain())
}

final class AppDelegate: NSObject, NSApplicationDelegate {
  @MainActor private let termination = AppTermination()
  @MainActor private var signalled = false
  private var signalSources: [DispatchSourceSignal] = []
  @MainActor var terminationReply: (Bool) -> Void = { NSApp.reply(toApplicationShouldTerminate: $0) }
  @MainActor var activityBeforeQuit: () async throws -> Bool = { try await AppModel.shared.environments.activityBeforeQuit() }
  @MainActor var localActivityBeforeQuit: () -> Bool = {
    AppModel.shared.sessions.values.contains { $0.runActive || $0.saving || $0.draftFinishTask != nil }
      || AppModel.shared.openChats.contains { $0.busy }
  }
  @MainActor var stopRuntime: () async -> Void = {
    if Updater.shared.installing {
      await AppModel.shared.environments.shutdownForUpdate()
    } else {
      await AppModel.shared.environments.shutdownForQuit()
    }
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    MainActor.assumeIsolated { AppPreferences.shared.quitAfterLastWindowClosed }
  }

  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    MainActor.assumeIsolated {
      termination.begin(
        activity: activityBeforeQuit,
        localActivity: localActivityBeforeQuit,
        confirm: {
          // A signal cannot be answered: stop in order without asking.
          if self.signalled { return true }
          let alert = NSAlert()
          alert.messageText = String(localized: "進行中の処理を停止して Tsugumi を終了しますか？")
          alert.informativeText = String(localized: "ほかのクライアントの処理も含め、実行・Agent・保存中の処理を停止し、実行環境を終了します。保存されていない変更は失われることがあります。")
          alert.addButton(withTitle: String(localized: "終了して停止"))
          alert.addButton(withTitle: String(localized: "キャンセル"))
          return alert.runModal() == .alertFirstButtonReturn
        },
        settleEdits: { self.signalled ? true : await self.settleEdits() },
        stop: {
          for session in AppModel.shared.sessions.values { session.endForQuit() }
          await ChatGPTSignInModel.cancelPendingSignIns()
          await self.stopRuntime()
        },
        reply: { allowed in
          if !allowed { Updater.shared.cancelRelaunch() }
          self.terminationReply(allowed)
        })
      return .terminateLater
    }
  }

  /// Keep the ordinary unsaved-editor choice when no operation is in flight.
  @MainActor private func settleEdits() async -> Bool {
    let dirty = AppModel.shared.sessions.values.filter(\.hasUnsavedEdits)
    if dirty.isEmpty { return true }
    let alert = NSAlert()
    alert.messageText = String(localized: "保存されていない変更があります")
    alert.informativeText = dirty.map { $0.scopeName([]) }.sorted().joined(separator: "、")
    alert.addButton(withTitle: String(localized: "保存して終了"))
    alert.addButton(withTitle: String(localized: "キャンセル"))
    alert.addButton(withTitle: String(localized: "保存せずに終了"))
    switch alert.runModal() {
    case .alertFirstButtonReturn:
      for s in dirty { _ = await s.finishDraftEdit(); await s.save() }
      if let failed = dirty.first(where: \.hasUnsavedEdits) {
        let a = NSAlert()
        a.messageText = String(localized: "保存できなかったため終了しませんでした")
        a.informativeText = "\(failed.scopeName([])): \(failed.saveError ?? "")"
        a.runModal()
        return false
      }
      return true
    case .alertThirdButtonReturn: return true
    default: return false
    }
  }

  func applicationDidFinishLaunching(_ notification: Notification) {
    // TERM and INT end the app the way Quit does, so the host service and the
    // VM stop in order with it instead of staying behind.
    for number in [SIGTERM, SIGINT] {
      signal(number, SIG_IGN)
      let source = DispatchSource.makeSignalSource(signal: number, queue: .global())
      source.setEventHandler { [weak self] in
        requestQuit { self?.signalled = true }
      }
      source.resume()
      signalSources.append(source)
    }
    // Launched with `swift run` there is no bundle: still behave as a regular app.
    NSApp.setActivationPolicy(.regular)
    // The Dock icon also when the executable is started directly (scripts,
    // `swift run` next to the repository's icon).
    var icon = Bundle.main.url(forResource: "AppIcon", withExtension: "icns")
    #if DEBUG
    if icon == nil { icon = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../Resources/AppIcon.icns") }
    #endif
    if let icon, let image = NSImage(contentsOf: icon) { NSApp.applicationIconImage = image }
    Task { @MainActor in
      AppPreferences.shared.applyTheme()
      SmokeTest.runIfRequested(AppModel.shared)
      UIScript.runIfRequested()
    }
  }

  /// The host has been asked to stop before AppKit completes termination.
  /// A downloaded update takes the app's place after orderly runtime shutdown.
  func applicationWillTerminate(_ notification: Notification) {
    MainActor.assumeIsolated { Updater.shared.installIfReady() }
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
          .modifier(ClosesWithProject(session: session))
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
  @FocusedValue(\.mainWindow) private var mainWindow
  @Environment(\.openWindow) private var openWindow

  private var onCanvas: Bool { canvasFocused ?? false }
  private var selected: CardKey? { tab?.selection.sorted().first }
  private func post(_ n: Notification.Name, _ info: [String: Any]? = nil) {
    NotificationCenter.default.post(name: n, object: tab?.id, userInfo: info)
  }
  /// The main window of the project in front (a Card's settings window acts on its project's).
  private var projectWindow: Workspace? {
    if let mainWindow, mainWindow.session != nil { return mainWindow }
    return session.flatMap { AppModel.shared.mainWindow(showing: $0) }
  }

  var body: some Commands {
    CommandGroup(replacing: .newItem) {
      // Each main window has its project: these leave the one in front alone, Home takes them.
      Button("新しいウインドウ") { openWindow(id: "main") }
        .keyboardShortcut("n", modifiers: [.command, .shift])
      Button("新しいプロジェクト…") { AppModel.shared.newProject(from: mainWindow, openWindow: openWindow) }.shortcut(.newProject)
      Button("プロジェクトを開く…") { AppModel.shared.chooseProject(from: mainWindow, openWindow: openWindow) }.shortcut(.openProject)
      Button("Home へ戻る") { if let w = projectWindow { AppModel.shared.leaveProject(in: w) } }
        .shortcut(.home).disabled(session == nil)
    }
    CommandGroup(replacing: .undoRedo) {
      Button("取り消す") { Task { await session?.undo() } }
        .keyboardShortcut("z")
        .disabled(session?.undoStack.isEmpty ?? true)
      Button("やり直す") { Task { await session?.redo() } }
        .keyboardShortcut("z", modifiers: [.command, .shift])
        .disabled(session?.redoStack.isEmpty ?? true)
    }
    CommandGroup(after: .saveItem) {
      Button("プロジェクトを複製…") { if let w = projectWindow { AppModel.shared.duplicateProject(in: w, openWindow: openWindow) } }
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
        .disabled(!(session?.canStopRun ?? false))
      Button("版の履歴…") { if let s = session { openWindow(id: "versions", value: ProjectRef(path: s.identity.url.path)) } }
        .keyboardShortcut("y", modifiers: [.command, .option])
        .disabled(session == nil)
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
          .disabled(!onCanvas || !(tab?.hasSelection ?? false))
        Divider()
        Button("全体表示") { post(.canvasFitAll) }.shortcut(.fitAll).disabled(!onCanvas)
        Button("100% で表示") { post(.canvasZoom, ["zoom": 0.0]) }.shortcut(.actualSize).disabled(!onCanvas)
        Button("拡大") { post(.canvasZoom, ["zoom": 1.2]) }.shortcut(.zoomIn).disabled(!onCanvas)
        Button("縮小") { post(.canvasZoom, ["zoom": 1 / 1.2]) }.shortcut(.zoomOut).disabled(!onCanvas)
        Button("整列") { if let s = session, let t = tab { Task { await s.arrange(t.scope) } } }
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
      // In a project Tsugumi is the chat; on Home, its panel in the corner.
      Button("ガイドのツグミに聞く") { if let w = workspace { w.showChat.toggle() } else { GuideController.shared.toggleFromMenu() } }
        .shortcut(.toggleGuide)
        .disabled(!GuideController.shared.available)
      Button("ログの表示切替") { workspace?.togglePanel(.log) }
        .shortcut(.toggleLog)
      Button("タイムラインの表示切替") { workspace?.togglePanel(.timeline) }
        .shortcut(.toggleTimeline)
      Button("ターミナルの表示切替") { workspace?.togglePanel(.terminal) }
        .shortcut(.toggleTerminal)
    }
    CommandGroup(after: .windowList) {
      OpenConsoleButton()
      OpenDebugPanelButton()
    }
    CommandGroup(after: .appInfo) {
      UpdateMenuItem()
    }
    CommandGroup(replacing: .help) {
      OpenExplainerButton()
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
  /// The demo list on Home (UI scripts).
  static let appShowDemos = Notification.Name("tsugumi.app.showDemos")
  /// Canvas commands (object: tab id).
  static let canvasDelete = Notification.Name("tsugumi.canvas.delete")
  static let canvasGroup = Notification.Name("tsugumi.canvas.group")
  static let canvasAddDraft = Notification.Name("tsugumi.canvas.addDraft")
  static let canvasWriteDraft = Notification.Name("tsugumi.canvas.writeDraft")
  static let canvasFitAll = Notification.Name("tsugumi.canvas.fitAll")
  /// As if the pointer came on a row of the version tree (object: the row; UI scripts).
  static let versionHover = Notification.Name("tsugumi.versions.hover")
  static let canvasAddNote = Notification.Name("tsugumi.canvas.addNote")
  static let canvasAddBuiltin = Notification.Name("tsugumi.canvas.addBuiltin")
  /// Moves a canvas (object: tab id) to a Card (userInfo "card").
  static let canvasReveal = Notification.Name("tsugumi.canvas.reveal")
  /// A Card was renamed (userInfo: from, to, project): views showing it follow.
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
struct MainWindowKey: FocusedValueKey { typealias Value = Workspace }

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
  /// The main window in front, with a project or on Home.
  var mainWindow: Workspace? {
    get { self[MainWindowKey.self] }
    set { self[MainWindowKey.self] = newValue }
  }
  /// The canvas has the keyboard focus (not a text field).
  var canvasFocused: Bool? {
    get { self[CanvasFocusedKey.self] }
    set { self[CanvasFocusedKey.self] = newValue }
  }
}
