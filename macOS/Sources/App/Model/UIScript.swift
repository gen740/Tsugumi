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
///   open definitions | workflow-settings
///   settings <category>             close settings
///   edit <card path> <param> <v>    external <card path> <field> <value>
///   capture <name>                  arrange
///   zoom <scale>                    fit
///   run | waitrun                   enter <scope path or ->
///   panel log|timeline              details <card path>
///   click <x> <y> [window title]    move <x> <y> [window title]
///   drag <x1> <y1> <x2> <y2> [window title]
///   save                            key delete|forwarddelete|escape
///   winsize <w> <h> <window title>  home | demos (Home's demo list)
///   open versions                   makerepo (the project becomes a repository)
///   add <template> <x> <y>          wire <index>   chatsample
///   chatsend <text>                 waitchat
///   explainer (Help > Tsugumi を知る)   scroll <x> <y> <dy> [window title]
///   marquee <x1> <y1> <x2> <y2>     (world units: what Shift + drag over them selects)
///   menu <item title>               (presses the menu bar's item; the app stays behind, so those that go by the key window are not for this)
///   newproject                      (the new project sheet, as ⌘N in the window steps act on)
///   openproject <folder>            (as the open panel of the window in front answers)
///   window <n>                      (later steps act on the n-th main window)
///   mainwindows                     (a line per main window: its project, connection, run, unsaved edits)
///   closewindow                     (as the window's close button)
///   offlineedit <card> <param> <v>  (every open project loses its environment and gets an edit; then the app quits, with them unsaved)
///   answer <n>                      (presses the n-th button of the next alert: a quit's question)
///   (points from the window's top-left; sent to the window like a real
///   mouse event, so AppKit and SwiftUI handle them as they would a click)
///
/// The app stays behind the user's work (macOS/tools/run-ui.sh opens it with
/// `open -g`): it never activates itself and its windows go to the back.
/// TSUGUMI_UI_FRONT=1 brings it to the front, for pictures with active colors.
@MainActor
@Observable
final class UIScript {
  static let shared = UIScript()

  /// A scripted check leaves the screen to the user (above).
  nonisolated static let staysBehind = ProcessInfo.processInfo.environment["TSUGUMI_UI_SCRIPT"] != nil
    && ProcessInfo.processInfo.environment["TSUGUMI_UI_FRONT"] == nil

  enum Request: Equatable {
    case card(CardRef), visual(CardRef), definitions(ProjectRef), versions(ProjectRef), workflowSettings(ProjectRef), settings, explainer
    case project(URL), newProject
  }

  var request: Request?
  /// Whether a canvas has the keyboard focus (reported by `state`).
  @ObservationIgnored var canvasFocused = false
  /// The main window steps act on (`window <n>`; the first one there is until then).
  @ObservationIgnored private weak var chosen: Workspace?
  var workspace: Workspace? { chosen ?? AppModel.shared.mainWindows.first }
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

  /// A line for the script's log from elsewhere in the app (what a step caused).
  func note(_ s: String) { say(s) }

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
      case "winsize":
        // winsize <width> <height> <window title>: another window's size
        let title = w.dropFirst(3).joined(separator: " ")
        if let win = NSApp.windows.first(where: { $0.isVisible && $0.title.contains(title) }),
           let width = Double(w[1]), let height = Double(w[2]) {
          var f = win.frame
          f.origin.y += f.height - height
          f.size = CGSize(width: width, height: height)
          win.setFrame(f, display: true, animate: false)
        }
      case "select": workspace?.active?.selection = [w[1]]
      case "makerepo": if let s = session { try? await s.makeRepository() }
      case "arrange": if let s = session, let t = workspace?.active { s.arrange(t.scope) }
      case "open":
        guard let ref else { break }
        switch w[1] {
        case "card":
          let cat = CardEditorCategory(rawValue: w.count > 3 ? w[3] : "general") ?? .general
          request = .card(CardRef(path: ref.path, card: w[2], category: cat))
        case "visual": request = .visual(CardRef(path: ref.path, card: w[2]))
        case "definitions": request = .definitions(ref)
        case "versions": request = .versions(ref)
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
        // Another client changes a Card's settings (its _card.json) while this window holds edits.
        if let s = session, let rpc = s.rpc, var card = s.confirmedDefinitions[w[1]] {
          card[w[2]] = .string(w.dropFirst(3).joined(separator: " "))
          _ = try? await rpc.save(baseRev: s.rev, opId: UUID(), origin: "script:other",
                                  changes: [Change.putDefinition(id: w[1], cardJSON: card.serialized(), metaJSON: s.confirmedDefinitionMeta[w[1]]?.serialized())])
        }
      case "marquee":
        // The items a Shift + drag over these world coordinates selects (the script cannot hold Shift).
        if let s = session, let t = workspace?.active, w.count > 4,
           let x1 = Double(w[1]), let y1 = Double(w[2]), let x2 = Double(w[3]), let y2 = Double(w[4]) {
          let items = s.marqueeItems(CGRect(x: min(x1, x2), y: min(y1, y2), width: abs(x2 - x1), height: abs(y2 - y1)), in: t.scope)
          t.selection = items.cards
          t.selectedNotes = items.notes
          t.selectedFrames = items.frames
          say("marquee cards=\(items.cards.count) notes=\(items.notes.count) frames=\(items.frames.count)")
        }
      case "explainer": request = .explainer
      case "home": if let w = workspace { AppModel.shared.leaveProject(in: w) }
      case "newproject": request = .newProject
      case "openproject": request = .project(URL(fileURLWithPath: w.dropFirst().joined(separator: " ")))
      case "window":
        let list = AppModel.shared.mainWindows
        if let n = Int(w[1]), list.indices.contains(n) {
          chosen = list[n]
          list[n].bringForward()  // (not while the app stays behind)
        } else { say("no main window \(w[1])") }
      case "mainwindows":
        for (i, m) in AppModel.shared.mainWindows.enumerated() {
          let s = m.session
          say("main \(i) title=\(m.window?.title ?? "-") project=\(s?.identity.url.lastPathComponent ?? "home") connected=\(s?.connected ?? false) "
              + "run=\(s?.runActive ?? false)/\(s?.runStatus?.state.label ?? "-") dirty=\(s?.dirty ?? false) key=\(m.window?.isKeyWindow ?? false) "
              + "closable=\(m.window?.standardWindowButton(.closeButton)?.isEnabled ?? false) sheet=\(m.window?.attachedSheet != nil)")
        }
        say("sessions \(AppModel.shared.sessions.values.map { $0.identity.url.lastPathComponent }.sorted())")
      case "answer":
        // answer <n>: the n-th button (1...) of the next alert, a moment after it comes up.
        let code = NSApplication.ModalResponse(rawValue: NSApplication.ModalResponse.alertFirstButtonReturn.rawValue + (Int(w[1]) ?? 1) - 1)
        // A timer: the quit may be asked from a main-actor task, whose alert keeps the main queue busy.
        let timer = Timer(timeInterval: 0.02, repeats: true) { t in
          MainActor.assumeIsolated {
            guard let alert = NSApp.modalWindow else { return }
            t.invalidate()
            let texts = alert.contentView?.subviews.compactMap { ($0 as? NSTextField)?.stringValue } ?? []
            self.say("alert: " + texts.joined(separator: " | "))
            alert.alphaValue = 0
            NSApp.stopModal(withCode: code)
          }
        }
        RunLoop.main.add(timer, forMode: .common)
      case "offlineedit":
        // offlineedit <card> <param> <value>: in every open project the environment
        // goes away and an edit follows, so the edits stay unsaved (a quit's question).
        let open = AppModel.shared.mainWindows.compactMap(\.session)
        for s in open { s.stopEnvironment() }
        try? await Task.sleep(for: .milliseconds(100))
        for s in open {
          s.setParam(w[1], w[2], parseParam(w[3], "uint64"))
          say("offlineedit \(s.identity.url.lastPathComponent) dirty=\(s.dirty)")
        }
        NSApp.terminate(nil)
      case "closewindow": workspace?.window?.performClose(nil)
      case "menu":
        let title = w.dropFirst().joined(separator: " ")
        func press(_ menu: NSMenu) -> Bool {
          for (i, item) in menu.items.enumerated() {
            if item.title == title {
              menu.update()
              say("menu \(title) enabled=\(item.isEnabled)")
              if item.isEnabled { menu.performActionForItem(at: i) }
              return true
            }
            if let sub = item.submenu, press(sub) { return true }
          }
          return false
        }
        if let bar = NSApp.mainMenu, !press(bar) { say("no menu item \(title)") }
      case "demos": NotificationCenter.default.post(name: .appShowDemos, object: nil)
      case "generate":
        // The canvas's "Card を生成": the Drafts of the shown scope go to the Agent.
        if let s = session, let t = workspace?.active {
          let drafts = s.cards(in: t.scope).compactMap { $0["id"]?.string }.map { key(t.scope + [$0]) }.filter { s.kind($0) == .draft }
          workspace?.showChat = true
          AppModel.shared.chat(for: s).generateDrafts(scope: t.scope, cards: drafts)
          say("generate \(drafts)")
        }
      case "presence":
        // presence [thought]: plays the Agent's scenes over the shown scope's
        // Cards along the flow (read, write out, thread the next wire), as
        // a save by the Agent would; `thought` adds sample thought lines.
        if let s = session, let t = workspace?.active {
          let ids = s.cards(in: t.scope).compactMap { $0["id"]?.string }
          let across = s.flow == .leftToRight
          let order = ids.sorted { let a = s.position(key(t.scope + [$0])), b = s.position(key(t.scope + [$1])); return across ? a.x < b.x : a.y < b.y }
          let p = s.presence
          p.name = { [weak s] k in s?.nodeData(k).title ?? k }
          p.turnStarted()
          for id in order where s.kind(key(t.scope + [id])) != .trigger {
            let k = key(t.scope + [id])
            p.enqueue(.visit(k))
            p.enqueue(.write(k, reveal: true))
            for c in s.connections(in: t.scope) where c["from"]?["card"]?.string == id {
              if let to = c["to"]?["card"]?.string, let fp = c["from"]?["port"]?.string, let tp = c["to"]?["port"]?.string {
                p.enqueue(.thread(AgentPresence.Wire(from: k, fromPort: fp, to: key(t.scope + [to]), toPort: tp)))
              }
            }
          }
          if w.count > 1 && w[1] == "thought" { p.think(String(localized: "上流から順に Card を書いて、糸でつないでいく")) }
          let end = p.beats.last?.end ?? Date()
          Task { @MainActor in
            try? await Task.sleep(for: .seconds(max(0, end.timeIntervalSinceNow)))
            p.turnEnded()
          }
          say("presence \(p.beats.count) scenes, \(String(format: "%.1f", end.timeIntervalSinceNow)) s")
        }
      case "presencelog":
        // Writes `bubble <seconds> <scene> | <line>` whenever the tsugumi's
        // bubble changes, for the rest of the script.
        if let s = session {
          let t0 = Date()
          Task { @MainActor [weak self] in
            var last: [String: String] = [:], lastError = "", lastTidy = ""
            while self?.log != nil {
              let now = Date()
              // The main bird and each helper's (named).
              for p in s.presences {
                let scene = p.current(at: now).map { "\($0.beat.beat)" } ?? "-"
                let text = (p.visible(at: now) ? p.line(at: now)?.text : nil) ?? "(none)"
                let who = p.label ?? "main"
                let entry = scene.prefix(60) + " | " + text
                if entry != last[who] {
                  self?.say(String(format: "bubble %.1f ", now.timeIntervalSince(t0)) + "[\(who)] " + entry)
                  last[who] = entry
                }
              }
              let t = AppModel.shared.chat(for: s).tidied
              if "\(t)" != lastTidy {
                self?.say(String(format: "tidy %.1f ", now.timeIntervalSince(t0)) + "moved \(t)")
                lastTidy = "\(t)"
              }
              let err = AppModel.shared.chat(for: s).thoughts.lastError ?? ""
              if err != lastError {
                self?.say("thought-error " + err.prefix(300))
                lastError = err
              }
              try? await Task.sleep(for: .milliseconds(250))
            }
          }
        }
      case "waitchat":
        // waitchat <minutes>: until the Agent's turn ends
        if let s = session {
          let chat = AppModel.shared.chat(for: s)
          let deadline = Date().addingTimeInterval((Double(w.count > 1 ? w[1] : "30") ?? 30) * 60)
          try? await Task.sleep(for: .seconds(2))
          while chat.busy && Date() < deadline { try? await Task.sleep(for: .seconds(2)) }
          say("chat busy=\(chat.busy) limit=\(chat.stoppedAtLimit) status=\(chat.status ?? "-")")
        }
      case "continue":
        if let s = session { AppModel.shared.chat(for: s).continueWork() }
      case "add":
        // add <template> <x> <y>: a built-in Card at a canvas (world) point
        if let s = session, let t = workspace?.active, let tmpl = DefinitionTemplate(rawValue: w[1]) {
          let k = s.addCard(template: tmpl, in: t.scope, at: CGPoint(x: Double(w[2]) ?? 0, y: Double(w[3]) ?? 0))
          t.selection = [k]
          say("added \(k)")
        }
      case "wire":
        // wire <index>: selects a connection of the shown scope
        workspace?.active?.selectedConnection = Int(w[1])
      case "chatsample":
        if let s = session {
          AppModel.shared.chat(for: s).showSample(question: "コラッツの漸化式を説明して", answer: """
          ## コラッツの漸化式
          $a_0 = n$ から始めて、次の規則で数列を作ります。

          $$
          a_{k+1} = \\begin{cases} a_k / 2 & a_k \\equiv 0 \\pmod 2 \\\\ 3a_k + 1 & a_k \\equiv 1 \\pmod 2 \\end{cases}
          $$

          - $n = 27$ では **111 手**で $1$ に着きます
          - 途中の最大値は $9232$ です

          | $n$ | 停止時間 |
          | ---: | ---: |
          | 6 | 8 |
          | 27 | 111 |

          `compare` Card の出力 `sequence` を `record` が CSV に書きます。
          """)
        }
      case "chatsend":
        // chatsend <text>: sends to the configured model (real provider)
        if let s = session { AppModel.shared.chat(for: s).send(w.dropFirst().joined(separator: " ")) }
      case "focus":
        // Gives the canvas the keyboard focus (as a click on it does).
        if let win = mainWindow { win.makeFirstResponder(win.contentView) }
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
          say("apply \(ok) conflicts=\(m.conflicts) unapplied=\(m.unapplied) description=\(m.meta?["description"]?.string ?? "")")
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
        // In front when asked for (the window buttons and selection in their active colors).
        if !Self.staysBehind {
          NSApp.activate()
          target?.makeKeyAndOrderFront(nil)
        }
        try? await Task.sleep(for: .milliseconds(300))
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
      case "scroll":
        // scroll <x> <y> <dy>: a wheel event at a point of the window (dy > 0 scrolls down), as a trackpad sends
        let title = w.dropFirst(4).joined(separator: " ")
        let win = title.isEmpty ? mainWindow : NSApp.windows.first { $0.isVisible && $0.title.contains(title) }
        guard let win, w.count >= 4, let x = Double(w[1]), let y = Double(w[2]), let dy = Double(w[3]),
              let cg = CGEvent(scrollWheelEvent2Source: nil, units: .pixel, wheelCount: 1, wheel1: Int32(-dy), wheel2: 0, wheel3: 0)
        else { say("no window for " + line); break }
        // An NSEvent made from a CGEvent has no window: its locationInWindow is
        // its screen location, y up from the primary screen's bottom. So the CG
        // location is the one that reads back as this point of the window.
        let primaryHeight = NSScreen.screens.first?.frame.maxY ?? 0
        cg.location = CGPoint(x: x, y: primaryHeight - (win.frame.height - y))
        guard let e = NSEvent(cgEvent: cg) else { say("no event for " + line); break }
        // Taken from the queue, so it is NSApp.currentEvent while views are hit-tested
        // (the canvas and the timeline ask, to take the wheel only).
        NSApp.postEvent(e, atStart: true)
        if let next = NSApp.nextEvent(matching: .scrollWheel, until: nil, inMode: .default, dequeue: true) { win.sendEvent(next) }
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
      case "key":
        // key delete | forwarddelete | escape: a key press to this app
        let codes: [String: CGKeyCode] = ["delete": 51, "forwarddelete": 117, "escape": 53]
        if let code = codes[w[1]] {
          say("key \(w[1]): canvas focused \(canvasFocused), selected wire \(workspace?.active?.selectedConnection.map(String.init) ?? "-")")
          // Through the window server to this process, as a real key press.
          for down in [true, false] {
            CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: down)?.postToPid(getpid())
            try? await Task.sleep(for: .milliseconds(40))
          }
        }
      case "save": if let s = session { await s.save() }
      case "quit": NSApp.terminate(nil)
      default: say("unknown " + line)
      }
      try? await Task.sleep(for: .milliseconds(500))
    }
    say("done")
  }
}
