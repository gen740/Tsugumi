import AppKit
import SwiftTerm
import SwiftUI
import TsugumiLauncher

/// A terminal in the open project's environment (inside the shared VM): bash
/// as the project's user in /project, with Nix and the Card tools. Kept apart
/// from the VM console, which shows how the environment started.
struct ProjectTerminalPanel: View {
  let session: ProjectSession
  var onClose: (() -> Void)?
  @State private var generation = 0
  @State private var ended: String?

  var body: some View {
    VStack(spacing: 0) {
      HStack(spacing: 8) {
        Image(systemName: "terminal")
        Text(session.identity.url.lastPathComponent).bold().lineLimit(1)
        Text(verbatim: "/project").font(.callout.monospaced()).foregroundStyle(.secondary)
        Text("実行環境内（このプロジェクトだけが見えます）").font(.caption).foregroundStyle(.secondary).lineLimit(1)
        Spacer()
        if let ended {
          Text(ended).font(.caption).foregroundStyle(.secondary).lineLimit(1)
        }
        Button { ended = nil; generation += 1 } label: { Label("新しいターミナル", systemImage: "plus") }
          .controlSize(.small)
          .help("ターミナルを開き直します（実行中のコマンドは終了します）")
        if let onClose {
          Button(action: onClose) { Image(systemName: "xmark").font(.caption) }
            .buttonStyle(.borderless).help("閉じる（下部のアイコンでも開閉できます）")
        }
      }
      .padding(.horizontal, 8).padding(.vertical, 4)
      Divider()
      if session.connected {
        TerminalHost(identity: session.identity, onEnd: { ended = $0 })
          .id(generation)
      } else {
        ContentUnavailableView("実行環境に接続していません", systemImage: "terminal",
                               description: Text("プロジェクトの実行環境が起動するとターミナルを使えます。"))
      }
    }
  }
}

/// SwiftTerm's view, connected to a terminal of tsugumi-vmd over vsock.
private struct TerminalHost: NSViewRepresentable {
  let identity: ProjectIdentity
  let onEnd: (String) -> Void

  func makeCoordinator() -> Coordinator { Coordinator(identity: identity, onEnd: onEnd) }

  func makeNSView(context: Context) -> TerminalView {
    let v = TerminalView(frame: .zero)
    v.terminalDelegate = context.coordinator
    v.font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
    context.coordinator.view = v
    context.coordinator.open()
    return v
  }

  func updateNSView(_ nsView: TerminalView, context: Context) {}

  static func dismantleNSView(_ nsView: TerminalView, coordinator: Coordinator) { coordinator.close() }

  final class Coordinator: NSObject, TerminalViewDelegate {
    let identity: ProjectIdentity
    let onEnd: (String) -> Void
    weak var view: TerminalView?
    private var fd: Int32 = -1
    private var sessionId: UInt64?
    private let writeQueue = DispatchQueue(label: "app.tsugumi.terminal.write")

    init(identity: ProjectIdentity, onEnd: @escaping (String) -> Void) {
      self.identity = identity
      self.onEnd = onEnd
    }

    func open() {
      let size = view?.getTerminal().getDims() ?? (cols: 80, rows: 24)
      Task { @MainActor in
        do {
          let (fd, sid) = try await AppModel.shared.environments.openTerminal(identity, rows: size.rows, cols: size.cols)
          self.fd = fd
          self.sessionId = sid
          read(fd)
        } catch {
          view?.feed(text: "\r\n\(String(localized: "ターミナルを開けませんでした")): \(error)\r\n")
          onEnd(String(localized: "開けませんでした"))
        }
      }
    }

    private func read(_ fd: Int32) {
      Thread.detachNewThread { [weak self] in
        var buf = [UInt8](repeating: 0, count: 16384)
        while true {
          let n = Darwin.read(fd, &buf, buf.count)
          if n <= 0 { break }
          let bytes = Array(buf[0..<n])
          DispatchQueue.main.async { self?.view?.feed(byteArray: bytes[...]) }
        }
        DispatchQueue.main.async {
          self?.view?.feed(text: "\r\n[\(String(localized: "ターミナルは終了しました"))]\r\n")
          self?.onEnd(String(localized: "終了しました"))
        }
      }
    }

    func close() {
      if fd >= 0 {
        shutdown(fd, SHUT_RDWR)
        Darwin.close(fd)
        fd = -1
      }
    }

    deinit { close() }

    // MARK: TerminalViewDelegate

    func send(source: TerminalView, data: ArraySlice<UInt8>) {
      let fd = self.fd
      guard fd >= 0 else { return }
      let bytes = Array(data)
      writeQueue.async {
        var off = 0
        while off < bytes.count {
          let n = bytes.withUnsafeBytes { Darwin.write(fd, $0.baseAddress! + off, bytes.count - off) }
          if n <= 0 { return }
          off += n
        }
      }
    }

    func sizeChanged(source: TerminalView, newCols: Int, newRows: Int) {
      guard let sid = sessionId else { return }
      Task { await AppModel.shared.environments.resizeTerminal(session: sid, rows: newRows, cols: newCols) }
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
}

/// The shared VM's console: how the execution environment was set up and
/// started (image, store disk, boot, Nix, network, supervisor) and what the
/// supervisor did since. Read-only; commands go to a project's terminal.
struct VMConsoleView: View {
  @State private var lines: [String] = []
  @State private var state: SharedVM.State = .stopped
  @State private var tokens: (UUID, UUID)?
  @State private var follow = true

  private var vm: SharedVM { AppModel.shared.environments.vm }

  var body: some View {
    VStack(spacing: 0) {
      HStack(spacing: 10) {
        Label("実行環境のコンソール", systemImage: "apple.terminal").font(.headline)
        Text(stateText).font(.callout).foregroundStyle(.secondary)
        Spacer()
        Toggle("末尾を追う", isOn: $follow).toggleStyle(.checkbox).controlSize(.small)
        Button("コピー") {
          NSPasteboard.general.clearContents()
          NSPasteboard.general.setString(lines.joined(separator: "\n"), forType: .string)
        }
        .controlSize(.small)
        Button("ログのフォルダ") { NSWorkspace.shared.activateFileViewerSelecting([vm.directory.appendingPathComponent("console.log")]) }
          .controlSize(.small)
          .help("console.log（ゲスト）、console.prev.log（前回の起動）、launcher.log（アプリの記録）")
      }
      .padding(8)
      Divider()
      ScrollViewReader { proxy in
        ScrollView {
          LazyVStack(alignment: .leading, spacing: 0) {
            ForEach(Array(lines.enumerated()), id: \.offset) { i, l in
              Text(verbatim: l.isEmpty ? " " : l)
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(color(l))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .id(i)
            }
          }
          .padding(8)
        }
        .overlay {
          if lines.isEmpty {
            ContentUnavailableView("まだ出力はありません", systemImage: "apple.terminal",
                                   description: Text("プロジェクトを開くと実行環境が起動し、その記録がここに表示されます。"))
          }
        }
        .onChange(of: lines.count) { _, n in if follow, n > 0 { proxy.scrollTo(n - 1, anchor: .bottom) } }
      }
    }
    .frame(minWidth: 520, minHeight: 300)
    .onAppear {
      lines = vm.console.snapshot
      let a = vm.console.observe { l in Task { @MainActor in lines.append(l); if lines.count > 5000 { lines.removeFirst(lines.count - 5000) } } }
      let b = vm.observe { s in Task { @MainActor in state = s } }
      tokens = (a, b)
    }
    .onDisappear {
      if let (a, b) = tokens {
        vm.console.stopObserving(a)
        vm.stopObserving(b)
      }
    }
  }

  private var stateText: String {
    switch state {
    case .stopped: return String(localized: "停止中")
    case .starting(let s): return String(localized: "起動中: \(EnvironmentOverlay.stageText(EnvironmentStage(vmStage: s).rawValue))")
    case .running: return String(localized: "稼働中")
    case .failed(let m): return String(localized: "失敗: \(m)")
    }
  }

  private func color(_ l: String) -> SwiftUI.Color {
    if l.hasPrefix("TSUGUMI-FAILED") || l.localizedCaseInsensitiveContains("error") || l.contains("failed") { return .red }
    if l.hasPrefix("TSUGUMI-STAGE") { return .accentColor }
    if l.hasPrefix("[tsugumi]") { return .secondary }
    return .primary
  }
}
