import AppKit
import Foundation
import Observation
import SwiftUI
import TsugumiRPC

/// Scripted UI checks: with TSUGUMI_UI_SCRIPT=<file> the app performs the
/// listed steps on its real windows and writes `capture <name> <window>` lines
/// to TSUGUMI_UI_LOG, pausing so an outside tool can take the picture.
///
///   wait <seconds>                  sidebar hide|show
///   size <width> <height>           chat show|hide
///   select <card path>              open card <card path> <category>
///   open draft <card path>          open definitions | workflow-settings
///   settings <category>             close settings
///   edit <card path> <param> <v>    external <card path> <field> <value>
///   capture <name>                  arrange
///   zoom <scale>                    fit
///   run | waitrun                   enter <scope path or ->
///   panel log|timeline              details <card path>
///   click <x> <y> [window title]    move <x> <y> [window title]
///   drag <x1> <y1> <x2> <y2> [window title]
///   save
///   (points from the window's top-left; sent to the window like a real
///   mouse event, so AppKit and SwiftUI handle them as they would a click)
@MainActor
@Observable
final class UIScript {
  static let shared = UIScript()

  enum Request: Equatable {
    case card(CardRef), draft(CardRef), definitions(ProjectRef), workflowSettings(ProjectRef), settings
  }

  var request: Request?
  @ObservationIgnored weak var workspace: Workspace?
  /// Card settings windows that are open (scripts edit their working copies).
  @ObservationIgnored var editors: [CardKey: CardEditorModel] = [:]
  @ObservationIgnored private var log: FileHandle?

  static func runIfRequested() {
    let env = ProcessInfo.processInfo.environment
    guard let script = env["TSUGUMI_UI_SCRIPT"], let text = try? String(contentsOfFile: script, encoding: .utf8) else { return }
    let logPath = env["TSUGUMI_UI_LOG"] ?? (script + ".log")
    FileManager.default.createFile(atPath: logPath, contents: nil)
    shared.log = FileHandle(forWritingAtPath: logPath)
    Task { await shared.run(text.split(separator: "\n").map(String.init)) }
  }

  private func say(_ s: String) {
    log?.write(Data((s + "\n").utf8))
    try? log?.synchronize()
  }

  private var mainWindow: NSWindow? {
    NSApp.windows.first { $0.isVisible && $0.frame.height > 300 && ($0.identifier?.rawValue.contains("main") ?? false) }
      ?? NSApp.windows.filter { $0.isVisible && $0.frame.height > 300 }.max { $0.frame.width < $1.frame.width }
  }

  private func run(_ lines: [String]) async {
    // Wait until the project is connected (a first line "nowait" starts at
    // once, e.g. to capture the loading screen).
    let waitForProject = lines.first?.trimmingCharacters(in: .whitespaces) != "nowait"
    for _ in 0..<600 where waitForProject {
      if let s = workspace?.session, s.connected { break }
      try? await Task.sleep(for: .milliseconds(100))
    }
    for line in lines {
      let w = line.split(separator: " ").map(String.init)
      guard let cmd = w.first, !cmd.hasPrefix("#"), cmd != "nowait" else { continue }
      say("step " + line)
      let session = workspace?.session
      let ref = session.map { ProjectRef(path: $0.identity.url.path) }
      switch cmd {
      case "wait": try? await Task.sleep(for: .milliseconds(Int((Double(w[1]) ?? 1) * 1000)))
      case "waitconnect":
        for _ in 0..<900 where !(workspace?.session?.connected ?? false) { try? await Task.sleep(for: .milliseconds(100)) }
      case "sidebar": workspace?.columns = w[1] == "hide" ? .detailOnly : .all
      case "chat": workspace?.showChat = w[1] == "show"
      case "size":
        if let win = mainWindow, let width = Double(w[1]), let height = Double(w[2]) {
          var f = win.frame
          f.size = CGSize(width: width, height: height)
          win.setFrame(f, display: true, animate: false)
        }
      case "select": workspace?.active?.selection = [w[1]]
      case "arrange": if let s = session, let t = workspace?.active { s.arrange(t.scope) }
      case "open":
        guard let ref else { break }
        switch w[1] {
        case "card":
          let cat = CardEditorCategory(rawValue: w.count > 3 ? w[3] : "general") ?? .general
          request = .card(CardRef(path: ref.path, card: w[2], category: cat))
        case "draft": request = .draft(CardRef(path: ref.path, card: w[2]))
        case "definitions": request = .definitions(ref)
        case "workflow-settings": request = .workflowSettings(ref)
        default: break
        }
      case "settings":
        UserDefaults.standard.set(w[1], forKey: "settings.category")
        request = .settings
      case "close":
        NSApp.windows.first { $0.title.contains(w.dropFirst().joined(separator: " ")) }?.performClose(nil)
      case "edit":
        if let s = session { s.setParam(w[1], w[2], parseParam(w[3], "uint64")) }
      case "external":
        // Another client changes the Card while this window holds edits.
        if let s = session, let rpc = s.rpc {
          var doc = s.confirmedProject
          func patch(_ g: inout JSONValue, _ p: ArraySlice<String>) {
            guard var cards = g["cards"]?.array, let i = cards.firstIndex(where: { $0["id"]?.string == p.first }) else { return }
            if p.count == 1 { cards[i][w[2]] = .string(w.dropFirst(3).joined(separator: " ")) } else if var inner = cards[i]["canvas"] {
              patch(&inner, p.dropFirst())
              cards[i]["canvas"] = inner
            }
            g["cards"] = .array(cards)
          }
          var canvas = doc["canvas"] ?? .object([])
          patch(&canvas, path(w[1])[...])
          doc["canvas"] = canvas
          _ = try? await rpc.save(baseRev: s.rev, opId: UUID(), origin: "script:other", changes: [Change.setProject(doc.serialized())])
        }
      case "editor":
        // editor <card> set <field> <value> | category <name> | apply
        guard let m = editors[w[1]] else { say("no editor for \(w[1])"); break }
        switch w[2] {
        case "set": m.definition?[w[3]] = .string(w.dropFirst(4).joined(separator: " "))
        case "category": m.category = CardEditorCategory(rawValue: w[3]) ?? m.category
        case "load":
          do { let t = try await m.text(w[3]); say("loaded \(w[3]) \(t.count) chars") } catch { say("load failed \(w[3]): \(error)") }
        case "tail":
          say("text \(w[3]) loaded=\(m.loadedText(w[3]) != nil) last=\(m.loadedText(w[3])?.split(separator: "\n").last ?? "-")")
        case "append":
          // editor <card> append <file path> <text>: edit a loaded file
          let p = w[3]
          if let t = m.loadedText(p) { m.texts[p] = t + w.dropFirst(4).joined(separator: " ") + "\n" } else { say("not loaded \(p)") }
        case "apply":
          let ok = await m.apply()
          say("apply \(ok) conflict=\(m.conflict != nil) unapplied=\(m.unapplied) description=\(m.definition?["description"]?.string ?? "")")
        default: break
        }
      case "state":
        let s = workspace?.session
        // (no endpoint details: the token stays out of logs)
        let env: String
        switch s?.environment {
        case .ready?: env = "ready"
        case .failed(let m, _, _)?: env = "failed: " + m
        case let other?: env = other.label
        case nil: env = "-"
        }
        say("state session=\(s != nil) env=\(env) connected=\(s?.connected ?? false) message=\(s?.connectionMessage ?? "-")")
        if let m = editors.values.first {
          say("editor card=\(m.cardKey) category=\(m.category) loads=\(m.loads) unapplied=\(m.unapplied)")
        }
      case "windows":
        for win in NSApp.windows where win.isVisible { say("window \(win.windowNumber) \(win.identifier?.rawValue ?? "-") \(win.title)") }
      case "capture":
        let windows = NSApp.windows.filter { $0.isVisible && $0.frame.height > 120 }
        let wanted = w.dropFirst(2).joined(separator: " ")
        let target = w.count > 2
          ? windows.first { $0.title.contains(wanted) || ($0.identifier?.rawValue.localizedCaseInsensitiveContains(wanted) ?? false) }
          : mainWindow
        say("capture \(w[1]) \(target?.windowNumber ?? -1)")
        try? await Task.sleep(for: .milliseconds(1800))
      case "zoom":
        if let s = session, let t = workspace?.active, let z = Double(w[1]) {
          let k = key(t.scope)
          var c = s.cameras[k] ?? (40, 40, 1)
          c.x *= z / c.zoom
          c.y *= z / c.zoom
          c.zoom = z
          s.cameras[k] = c
        }
      case "fit": NotificationCenter.default.post(name: .canvasFitAll, object: workspace?.active?.id)
      case "run": if let s = session, let t = workspace?.active { RunActions.runAll(s, scope: t.scope) }
      case "waitrun":
        try? await Task.sleep(for: .seconds(1))
        for _ in 0..<6000 where session?.runActive == true { try? await Task.sleep(for: .milliseconds(200)) }
        say("run \(session?.runStatus?.state.label ?? "-")")
      case "enter": workspace?.active?.enter(w[1] == "-" ? [] : path(w[1]))
      case "panel": workspace?.togglePanel(w[1] == "timeline" ? .timeline : .log)
      case "details":
        NotificationCenter.default.post(name: .canvasShowDetails, object: workspace?.active?.id, userInfo: ["card": w[1]])
      case "click", "move":
        let title = w.dropFirst(3).joined(separator: " ")
        let win = title.isEmpty ? mainWindow : NSApp.windows.first { $0.isVisible && $0.title.contains(title) }
        guard let win, let x = Double(w[1]), let y = Double(w[2]) else { say("no window for " + line); break }
        let p = NSPoint(x: x, y: win.frame.height - y)
        func event(_ t: NSEvent.EventType) -> NSEvent? {
          NSEvent.mouseEvent(with: t, location: p, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
                             windowNumber: win.windowNumber, context: nil, eventNumber: 0, clickCount: 1, pressure: 1)
        }
        if cmd == "move" {
          if let e = event(.mouseMoved) { win.sendEvent(e) }
        } else {
          if let d = event(.leftMouseDown) { win.sendEvent(d) }
          try? await Task.sleep(for: .milliseconds(60))
          if let u = event(.leftMouseUp) { win.sendEvent(u) }
        }
      case "drag":
        let title = w.dropFirst(5).joined(separator: " ")
        let win = title.isEmpty ? mainWindow : NSApp.windows.first { $0.isVisible && $0.title.contains(title) }
        guard let win, w.count >= 5, let x1 = Double(w[1]), let y1 = Double(w[2]), let x2 = Double(w[3]), let y2 = Double(w[4])
        else { say("no window for " + line); break }
        func event(_ t: NSEvent.EventType, _ x: Double, _ y: Double) -> NSEvent? {
          NSEvent.mouseEvent(with: t, location: NSPoint(x: x, y: win.frame.height - y), modifierFlags: [],
                             timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: win.windowNumber, context: nil,
                             eventNumber: 0, clickCount: 1, pressure: 1)
        }
        if let d = event(.leftMouseDown, x1, y1) { win.sendEvent(d) }
        for i in 1...12 {
          try? await Task.sleep(for: .milliseconds(30))
          let f = Double(i) / 12
          if let e = event(.leftMouseDragged, x1 + (x2 - x1) * f, y1 + (y2 - y1) * f) { win.sendEvent(e) }
        }
        if let u = event(.leftMouseUp, x2, y2) { win.sendEvent(u) }
      case "save": if let s = session { await s.save() }
      case "quit": NSApp.terminate(nil)
      default: say("unknown " + line)
      }
      try? await Task.sleep(for: .milliseconds(500))
    }
    say("done")
  }
}
