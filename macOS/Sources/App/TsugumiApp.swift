import AppKit
import SwiftUI

@main
struct TsugumiApp: App {
  @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
  private let model = AppModel.shared

  var body: some Scene {
    WindowGroup("Tsugumi", id: "home") {
      HomeView()
        .environment(model)
        .frame(minWidth: 760, minHeight: 520)
    }
    WindowGroup("Project", id: "project", for: URL.self) { $url in
      if let url {
        ProjectWindow(url: url)
          .environment(model)
          .frame(minWidth: 1100, minHeight: 700)
      }
    }
    .commands { ProjectCommands() }
    Settings {
      AgentSettingsView()
        .frame(width: 520, height: 360)
    }
  }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
  func applicationDidFinishLaunching(_ notification: Notification) {
    // Launched with `swift run` there is no bundle: still behave as a regular app.
    NSApp.setActivationPolicy(.regular)
    NSApp.activate(ignoringOtherApps: true)
  }
}

/// Menu commands routed to the focused project window.
struct ProjectCommands: Commands {
  @FocusedValue(\.projectSession) private var session

  var body: some Commands {
    CommandGroup(replacing: .undoRedo) {
      Button("Undo") { session?.undo() }
        .keyboardShortcut("z")
        .disabled(session?.undoStack.isEmpty ?? true)
      Button("Redo") { session?.redo() }
        .keyboardShortcut("z", modifiers: [.command, .shift])
        .disabled(session?.redoStack.isEmpty ?? true)
    }
    CommandGroup(after: .saveItem) {
      Button("Save") { if let s = session { Task { await s.save() } } }
        .keyboardShortcut("s")
        .disabled(!(session?.dirty ?? false))
    }
  }
}

struct ProjectSessionKey: FocusedValueKey {
  typealias Value = ProjectSession
}

extension FocusedValues {
  var projectSession: ProjectSession? {
    get { self[ProjectSessionKey.self] }
    set { self[ProjectSessionKey.self] = newValue }
  }
}
