import AppKit
import SwiftUI
import TsugumiLauncher

/// Where a project goes among the main windows. Each main window shows one
/// project (or Home); a project is in one main window at a time.
enum MainWindowRouting {
  struct Window: Equatable {
    let id: UUID
    /// The project shown (ProjectIdentity.id); nil: Home.
    let project: String?
  }

  enum Target: Equatable {
    /// The project is already open there: that window comes to the front.
    case front(UUID)
    /// The asking window shows Home: it takes the project.
    case here
    case newWindow
  }

  /// `project` nil: a new project (no window has it yet).
  static func target(project: String?, from: UUID?, windows: [Window]) -> Target {
    if let project, let open = windows.first(where: { $0.project == project }) { return .front(open.id) }
    if let from, let asking = windows.first(where: { $0.id == from }), asking.project == nil { return .here }
    return .newWindow
  }
}

/// What a new main window does first (it cannot be told before it exists).
enum MainWindowIntent: Equatable {
  case open(URL)
  case newProject
}

extension AppModel {
  func register(_ w: Workspace) {
    if !mainWindows.contains(where: { $0 === w }) { mainWindows.append(w) }
  }

  /// The main window nearest the front (a question for the whole app is asked there).
  var frontMainWindow: Workspace? {
    let order = NSApp.orderedWindows
    return mainWindows.min { a, b in
      (order.firstIndex { $0 === a.window } ?? .max) < (order.firstIndex { $0 === b.window } ?? .max)
    }
  }

  /// The main window showing a project (a detail window's menu commands act there).
  func mainWindow(showing s: ProjectSession) -> Workspace? {
    mainWindows.first { $0.session === s }
  }

  func takeWindowIntent() -> MainWindowIntent? {
    pendingWindowIntents.isEmpty ? nil : pendingWindowIntents.removeFirst()
  }

  /// Opens a project: where it already is, else in the asking window's Home, else in a new window.
  func openProject(_ url: URL, from w: Workspace?, openWindow: OpenWindowAction) {
    let id: String
    do {
      id = try ProjectIdentity(url: url).id
    } catch {
      report("\(error)", in: w)
      return
    }
    let windows = mainWindows.map { MainWindowRouting.Window(id: $0.id, project: $0.session?.identity.id) }
    switch MainWindowRouting.target(project: id, from: w?.id, windows: windows) {
    case .front(let open):
      mainWindows.first { $0.id == open }?.bringForward()
    case .here:
      if let w { show(url, in: w) }
    case .newWindow:
      pendingWindowIntents.append(.open(url))
      openWindow(id: "main")
    }
  }

  /// The new project sheet: on the asking window's Home, else on a new window's.
  func newProject(from w: Workspace?, openWindow: OpenWindowAction) {
    if let w, w.session == nil {
      w.requestNewProject = true
    } else {
      pendingWindowIntents.append(.newProject)
      openWindow(id: "main")
    }
  }

  func chooseProject(from w: Workspace?, openWindow: OpenWindowAction) {
    let p = NSOpenPanel()
    p.canChooseDirectories = true
    p.canChooseFiles = false
    p.message = String(localized: "tsugumi.json を含むフォルダを選んでください")
    guard p.runModal() == .OK, let url = p.url else { return }
    openProject(url, from: w, openWindow: openWindow)
  }

  /// A copy of the window's project (its saved document and sources), opened in a window of its own.
  func duplicateProject(in w: Workspace, openWindow: OpenWindowAction) {
    guard let s = w.session else { return }
    let p = NSSavePanel()
    p.nameFieldStringValue = s.identity.url.lastPathComponent + " copy"
    p.message = String(localized: "複製を作る場所を選んでください（保存済みの内容を複製します）")
    guard p.runModal() == .OK, let url = p.url else { return }
    Task {
      do {
        let copy = try await duplicateProject(from: s.identity.url, to: url)
        openProject(copy, from: w, openWindow: openWindow)
      } catch {
        w.alert = String(localized: "複製できませんでした: ") + "\(error)"
      }
    }
  }

  /// Back to Home in this window: nothing may be running. What cannot be saved
  /// is not left behind silently: the window asks whether to give it up.
  func leaveProject(in w: Workspace) {
    guard let s = w.session else { return }
    if s.runActive {
      w.alert = String(localized: "実行中はプロジェクトを切り替えられません。停止するか終了を待ってください。")
      return
    }
    Task {
      if s.dirty { await s.save() }
      if s.dirty {
        w.unsavedReason = s.saveError ?? s.connectionMessage ?? String(localized: "変更を保存できませんでした。")
        return
      }
      w.close()
      await release(s)
    }
  }

  /// The answer to that question: the changes go, and the window goes back to Home.
  func discardAndLeave(in w: Workspace) {
    guard let s = w.session else { return }
    s.discardDraft()
    w.close()
    Task { await release(s) }
  }

  /// A main window closed with its project: saved, then given back like leaving it.
  /// The other windows and their projects are not touched.
  func windowClosed(_ w: Workspace) {
    mainWindows.removeAll { $0 === w }
    guard let s = w.session else { return }
    w.close()
    Task {
      if s.dirty { await s.save() }
      await release(s)
    }
  }

  func show(_ url: URL, in w: Workspace) {
    do {
      let s = try session(for: url)
      hold(s)
      w.open(s)
    } catch {
      report("\(error)", in: w)
    }
  }

  /// A failure to open: Home shows it in its list, a project's window in its alert.
  private func report(_ message: String, in w: Workspace?) {
    if let w, w.session != nil { w.alert = message } else { error = message }
  }
}

extension Workspace {
  /// To the front, also out of the Dock or from behind another tab.
  func bringForward() {
    guard let window else { return }
    NSApp.activate()
    if window.isMiniaturized { window.deminiaturize(nil) }
    window.makeKeyAndOrderFront(nil)
  }
}

/// Tells which NSWindow a view is in (no window of a scene is named by SwiftUI).
struct WindowReader: NSViewRepresentable {
  let onWindow: (NSWindow) -> Void

  final class Probe: NSView {
    var onWindow: ((NSWindow) -> Void)?
    override func viewDidMoveToWindow() {
      super.viewDidMoveToWindow()
      if let window { onWindow?(window) }
    }
  }

  func makeNSView(context: Context) -> Probe {
    let v = Probe()
    v.onWindow = onWindow
    return v
  }

  func updateNSView(_ v: Probe, context: Context) { v.onWindow = onWindow }
}

/// A project's other windows (a Card's settings, a child workflow, the chat)
/// close when it closes: what they show is gone with its connection.
struct ClosesWithProject: ViewModifier {
  let session: ProjectSession?
  @Environment(\.dismiss) private var dismiss

  private var open: Bool { session.map { AppModel.shared.sessions[$0.identity.id] === $0 } ?? true }

  func body(content: Content) -> some View {
    content.onChange(of: open) { _, open in if !open { dismiss() } }
  }
}
