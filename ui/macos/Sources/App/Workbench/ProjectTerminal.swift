import AppKit
import SwiftTerm
import SwiftUI

/// A terminal in the open project's environment (inside the shared VM): bash
/// as the project's user in /project, with Nix and the Card tools. Kept apart
/// from the VM console, which shows how the environment started.
///
/// The panel only shows `session.terminals`: the tabs and their shells belong
/// to the project session, so hiding the panel or rebuilding this view leaves
/// running commands alone.
struct ProjectTerminalPanel: View {
  let session: ProjectSession
  var onClose: (() -> Void)?

  private var terminals: ProjectTerminals { session.terminals }

  var body: some View {
    VStack(spacing: 0) {
      HStack(spacing: 8) {
        Image(systemName: "terminal")
        Text(session.identity.url.lastPathComponent).bold().lineLimit(1)
        Text(verbatim: "/project").font(.callout.monospaced()).foregroundStyle(.secondary)
        Text("実行環境内（このプロジェクトだけが見えます）").font(.caption).foregroundStyle(.secondary).lineLimit(1)
        Spacer()
        Button { terminals.open() } label: { Label("新しいターミナル", systemImage: "plus") }
          .controlSize(.small)
          .disabled(!terminals.canOpen)
          .help(terminals.canOpen ? String(localized: "別のターミナルを開きます（実行中のコマンドは続きます）")
                                  : String(localized: "ターミナルは同時に \(ProjectTerminals.limit) 個までです"))
        if let onClose {
          Button(action: onClose) { Image(systemName: "xmark").font(.caption) }
            .buttonStyle(.borderless).help("パネルを隠します（ターミナルは開いたままで、実行中のコマンドは続きます）")
        }
      }
      .padding(.horizontal, 8).padding(.vertical, 4)
      ScrollView(.horizontal) {
        HStack(spacing: 4) {
          ForEach(terminals.tabs) { tab in
            HStack(spacing: 4) {
              Button { terminals.select(tab.number) } label: {
                Text("ターミナル \(tab.number)")
                if let ended = tab.ended { Text(ended).foregroundStyle(.secondary) }
              }
              .buttonStyle(.borderless)
              .accessibilityAddTraits(terminals.selection == tab.number ? .isSelected : [])
              Button { terminals.close(tab.number) } label: { Image(systemName: "xmark").font(.caption2) }
                .buttonStyle(.borderless).help("ターミナルを閉じます（実行中のコマンドは終了します）")
            }
            .padding(.horizontal, 8).padding(.vertical, 5)
            .background(terminals.selection == tab.number ? Color.accentColor.opacity(0.15) : .clear, in: RoundedRectangle(cornerRadius: 5))
          }
        }.padding(.horizontal, 8)
      }.fixedSize(horizontal: false, vertical: true)
      Divider()
      if terminals.tabs.isEmpty {
        ContentUnavailableView("ターミナルがありません", systemImage: "terminal",
                               description: Text("新しいターミナルを開いてください。"))
      } else if session.connected {
        // Every tab stays mounted, so an unselected one keeps the panel's size.
        ZStack {
          ForEach(terminals.tabs) { tab in
            TerminalHost(terminal: tab, active: terminals.selection == tab.number)
              .opacity(terminals.selection == tab.number ? 1 : 0)
              .allowsHitTesting(terminals.selection == tab.number)
              .accessibilityHidden(terminals.selection != tab.number)
          }
        }
      } else {
        ContentUnavailableView("実行環境への接続を待っています", systemImage: "terminal",
                               description: Text("接続するとターミナルが自動で開きます。"))
      }
    }
    .onAppear { terminals.show() }
  }
}

/// A project's terminal tabs and their shells, owned by its `ProjectSession`.
/// Views come and go; a shell ends only when its tab is closed, the shell
/// exits, the environment goes away, or the project is closed.
@MainActor
@Observable
final class ProjectTerminals {
  /// At most this many terminals per project window.
  static let limit = 8

  private(set) var tabs: [ProjectTerminal] = []
  /// The selected tab's number; 0 when there is none.
  private(set) var selection = 0
  @ObservationIgnored private var nextNumber = 1
  @ObservationIgnored private var connected = false
  @ObservationIgnored private var shown = false
  @ObservationIgnored private let identity: ProjectIdentity
  @ObservationIgnored private let openTerminal: TerminalConnection.Open

  init(identity: ProjectIdentity, openTerminal: @escaping TerminalConnection.Open) {
    self.identity = identity
    self.openTerminal = openTerminal
  }

  var canOpen: Bool { tabs.count < Self.limit }

  /// The panel came on screen. The first time, that opens the first terminal;
  /// a project whose terminal is never shown starts no shell.
  func show() {
    guard !shown else { return }
    shown = true
    open()
  }

  /// Adds a tab and selects it. Numbers are never reused, so a label always
  /// names the same shell. Refused at the limit.
  @discardableResult
  func open() -> Bool {
    guard canOpen else { return false }
    let number = nextNumber
    nextNumber += 1
    let tab = ProjectTerminal(number: number, identity: identity, openTerminal: openTerminal,
                              acceptsInput: { [weak self] in MainActor.assumeIsolated { self?.selection == number } })
    tabs.append(tab)
    selection = number
    if connected { tab.connect() }
    return true
  }

  func select(_ number: Int) {
    if tabs.contains(where: { $0.number == number }) { selection = number }
  }

  /// Ends the tab's shell. Closing the selected tab selects the one that
  /// takes its place, or the last one.
  func close(_ number: Int) {
    guard let index = tabs.firstIndex(where: { $0.number == number }) else { return }
    tabs.remove(at: index).close()
    if selection == number { selection = tabs.isEmpty ? 0 : tabs[min(index, tabs.count - 1)].number }
  }

  /// The session's connection to the environment changed. Connecting opens a
  /// shell for every tab without one (a tab made while waiting, or one whose
  /// shell ended) and clears its exit label; shells still open are kept.
  func environment(connected now: Bool) {
    guard now != connected else { return }
    connected = now
    guard now else { return }
    for tab in tabs where !tab.live { tab.connect() }
  }

  /// The project was closed: every shell ends.
  func closeAll() {
    for tab in tabs { tab.close() }
    tabs = []
    selection = 0
  }
}

/// One terminal tab: its shell connection, its exit label and, once shown,
/// the SwiftTerm view holding its scrollback.
@MainActor
@Observable
final class ProjectTerminal: Identifiable {
  let number: Int
  nonisolated var id: Int { number }
  /// Why the shell is gone ("ended", "could not open"); nil while it runs.
  private(set) var ended: String?
  @ObservationIgnored let connection: TerminalConnection
  /// Made when the tab is first shown, and kept: output received before then
  /// waits in the connection.
  @ObservationIgnored private(set) lazy var view: TerminalView = {
    let view = TerminalView(frame: .zero)
    view.font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
    connection.attach(view)
    return view
  }()

  init(number: Int, identity: ProjectIdentity, openTerminal: @escaping TerminalConnection.Open,
       acceptsInput: @escaping () -> Bool = { true }) {
    self.number = number
    connection = TerminalConnection(identity: identity, onEnd: { _ in }, openTerminal: openTerminal)
    connection.acceptsInput = acceptsInput
    connection.onEnd = { [weak self] ended in MainActor.assumeIsolated { self?.ended = ended } }
  }

  var live: Bool { connection.live }

  func connect() {
    guard !connection.live else { return }
    ended = nil
    connection.open()
  }

  func close() { connection.close() }
}

/// Shows a tab's SwiftTerm view. The view belongs to the tab, not to this
/// host: it is put into a container here and taken out again when the host
/// goes away, with its shell and scrollback untouched. When two windows show
/// the same project, the terminal appears in the one that mounted it last.
struct TerminalHost: NSViewRepresentable {
  let terminal: ProjectTerminal
  let active: Bool

  final class Coordinator { var active = false }

  func makeCoordinator() -> Coordinator { Coordinator() }

  func makeNSView(context: Context) -> NSView {
    let container = NSView(frame: .zero)
    adopt(into: container)
    return container
  }

  func updateNSView(_ container: NSView, context: Context) {
    let view = terminal.view
    if view.superview == nil { adopt(into: container) }
    if view.superview === container {
      if active && !context.coordinator.active, view.window?.isKeyWindow == true {
        view.window?.makeFirstResponder(view)
      } else if !active, view.window?.firstResponder === view {
        view.window?.makeFirstResponder(nil)
      }
    }
    context.coordinator.active = active
  }

  static func dismantleNSView(_ container: NSView, coordinator: Coordinator) {
    for view in container.subviews { view.removeFromSuperview() }
  }

  private func adopt(into container: NSView) {
    let view = terminal.view
    view.removeFromSuperview()
    view.frame = container.bounds
    view.autoresizingMask = [.width, .height]
    container.addSubview(view)
  }
}

/// A shell of tsugumi-vmd over vsock, and the SwiftTerm view (if any) that
/// shows it. Used on the main thread; reading and writing happen on their own
/// threads, each with a descriptor of its own.
final class TerminalConnection: NSObject, TerminalViewDelegate {
  typealias Open = (ProjectIdentity, Int, Int) async throws -> (fd: Int32, session: UInt64)
  /// Resizes a session's PTY: (session, rows, cols).
  typealias Resize = (UInt64, Int, Int) async throws -> Void
  struct Size: Equatable { var rows: Int, cols: Int }

  let identity: ProjectIdentity
  var onEnd: (String) -> Void
  /// Asked before every write: an unselected tab takes no keyboard input.
  var acceptsInput: () -> Bool = { true }
  private(set) weak var view: TerminalView?
  /// Output received while no view is attached (bounded; the oldest goes first).
  private var backlog: [UInt8] = []
  private static let backlogLimit = 1 << 20
  private var fd: Int32 = -1
  private var sessionId: UInt64?
  /// Bumped by every open and close: what an earlier shell still delivers
  /// (a late connection, output, its end) is recognised and dropped.
  private var generation = 0
  /// Opening or open.
  private(set) var live = false
  var opening: Task<Void, Never>?
  /// The size the shell's PTY has: opened with, or last resized to.
  private var ptySize: Size?
  /// The view's latest size, sent by `resizing`.
  private var wantedSize: Size?
  /// Sends resizes one at a time until the PTY has `wantedSize`.
  var resizing: Task<Void, Never>?
  /// The last resize that failed, kept until one succeeds.
  private(set) var resizeFailure: String?
  private let openTerminal: Open
  private let resizeTerminal: Resize
  let writeQueue = DispatchQueue(label: "app.tsugumi.terminal.write")

  init(identity: ProjectIdentity, onEnd: @escaping (String) -> Void,
       openTerminal: @escaping Open = { identity, rows, cols in
         try await AppModel.shared.environments.openTerminal(identity, rows: rows, cols: cols)
       },
       resizeTerminal: @escaping Resize = { session, rows, cols in
         try await AppModel.shared.environments.resizeTerminal(session: session, rows: rows, cols: cols)
       }) {
    self.identity = identity
    self.onEnd = onEnd
    self.openTerminal = openTerminal
    self.resizeTerminal = resizeTerminal
  }

  func attach(_ view: TerminalView) {
    self.view = view
    view.terminalDelegate = self
    if !backlog.isEmpty {
      view.feed(byteArray: backlog[...])
      backlog = []
    }
    syncSize()
  }

  private func feed(_ bytes: [UInt8]) {
    if let view { view.feed(byteArray: bytes[...]); return }
    backlog.append(contentsOf: bytes)
    if backlog.count > Self.backlogLimit { backlog.removeFirst(backlog.count - Self.backlogLimit) }
  }

  func open() {
    guard !live else { return }
    generation += 1
    let current = generation
    live = true
    let size = laidOutSize() ?? Size(rows: 24, cols: 80)
    opening = Task { @MainActor in
      do {
        let (fd, sid) = try await openTerminal(identity, size.rows, size.cols)
        // A tab can close while the VM is still opening its shell.
        guard current == generation else {
          shutdown(fd, SHUT_RDWR)
          Darwin.close(fd)
          return
        }
        // Writing to a shell that has gone must fail, not raise SIGPIPE.
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        self.fd = fd
        self.sessionId = sid
        ptySize = size
        read(fd, current)
        // The view may have been laid out, or resized, while the shell was
        // opening: those sizes reached no session.
        syncSize()
      } catch {
        guard current == generation else { return }
        live = false
        feed(Array("\r\n\(String(localized: "ターミナルを開けませんでした")): \(error)\r\n".utf8))
        onEnd(String(localized: "開けませんでした"))
      }
    }
  }

  private func read(_ socket: Int32, _ generation: Int) {
    // The reader owns a duplicate so a closed tab's descriptor cannot be
    // reused underneath it by a newly opened tab.
    let fd = dup(socket)
    guard fd >= 0 else { close(); onEnd(String(localized: "開けませんでした")); return }
    Thread.detachNewThread { [weak self] in
      defer { Darwin.close(fd) }
      var buf = [UInt8](repeating: 0, count: 16384)
      while true {
        let n = Darwin.read(fd, &buf, buf.count)
        if n <= 0 { break }
        let bytes = Array(buf[0..<n])
        DispatchQueue.main.async {
          guard let self, self.generation == generation else { return }
          self.feed(bytes)
        }
      }
      DispatchQueue.main.async {
        guard let self, self.generation == generation else { return }
        self.feed(Array("\r\n[\(String(localized: "ターミナルは終了しました"))]\r\n".utf8))
        self.close()
        self.onEnd(String(localized: "終了しました"))
      }
    }
  }

  /// Ends the shell: vmd hangs it up when its connection closes.
  func close() {
    generation += 1
    live = false
    sessionId = nil
    ptySize = nil
    wantedSize = nil
    if fd >= 0 {
      shutdown(fd, SHUT_RDWR)
      Darwin.close(fd)
      fd = -1
    }
  }

  deinit { close() }

  // MARK: TerminalViewDelegate

  func send(source: TerminalView, data: ArraySlice<UInt8>) { send(Array(data)) }

  func send(_ bytes: [UInt8]) {
    guard self.fd >= 0, acceptsInput() else { return }
    // The queued write owns a duplicate, as the reader does.
    let fd = dup(self.fd)
    guard fd >= 0 else { return }
    writeQueue.async {
      defer { Darwin.close(fd) }
      var off = 0
      while off < bytes.count {
        let n = bytes.withUnsafeBytes { Darwin.write(fd, $0.baseAddress! + off, bytes.count - off) }
        if n <= 0 { return }
        off += n
      }
    }
  }

  func sizeChanged(source: TerminalView, newCols: Int, newRows: Int) {
    resize(to: Size(rows: newRows, cols: newCols))
  }

  /// The attached view's size, once it has been laid out.
  private func laidOutSize() -> Size? {
    guard let view, !view.bounds.isEmpty else { return nil }
    let dims = view.getTerminal().getDims()
    return Size(rows: dims.rows, cols: dims.cols)
  }

  private func syncSize() {
    if let size = laidOutSize() { resize(to: size) }
  }

  /// Asks for the PTY to have `size`. Resizes go one at a time, each with the
  /// latest size, so quick changes end at the last one. A failure is logged
  /// and kept; the next size change tries again.
  private func resize(to size: Size) {
    wantedSize = size
    guard resizing == nil else { return }
    resizing = Task { @MainActor in
      while let sid = sessionId, let wanted = wantedSize, wanted != ptySize {
        let current = generation
        do {
          try await resizeTerminal(sid, wanted.rows, wanted.cols)
          if current == generation {
            ptySize = wanted
            resizeFailure = nil
          }
        } catch {
          // A shell that closed meanwhile; a new one is synced on its own.
          guard current == generation else { continue }
          resizeFailure = String(describing: error)
          NSLog("Tsugumi terminal resize to \(wanted.cols)x\(wanted.rows) failed: \(error)")
          break
        }
      }
      resizing = nil
    }
  }

  func setTerminalTitle(source: TerminalView, title: String) {}
  func hostCurrentDirectoryUpdate(source: TerminalView, directory: String?) {}
  func scrolled(source: TerminalView, position: Double) {}
  func requestOpenLink(source: TerminalView, link: String, params: [String: String]) {
    if let url = URL(string: link), ["http", "https"].contains(url.scheme) { NSWorkspace.shared.open(url) }
  }
  func bell(source: TerminalView) { NSSound.beep() }
  func clipboardCopy(source: TerminalView, content: Data) {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(String(decoding: content, as: UTF8.self), forType: .string)
  }
  func iTermContent(source: TerminalView, content: ArraySlice<UInt8>) {}
  func rangeChanged(source: TerminalView, startY: Int, endY: Int) {}
}
