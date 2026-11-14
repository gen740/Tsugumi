import AppKit
import SwiftUI
import TsugumiLauncher
import TsugumiRPC

/// Tabs of one project (the root workflow and opened child workflows) and the
/// workbench of the active tab.
struct WorkflowTabs: View {
  @Environment(AppModel.self) private var model
  @Bindable var workspace: Workspace
  @AppStorage("chat.width") private var chatWidth = 480.0
  let session: ProjectSession
  let onSwitchProject: (() -> Void)?
  @Environment(\.guideWindow) private var guideWindow
  @Environment(\.controlActiveState) private var activeState

  /// What changes in the project that a demo tour follows.
  private var runKey: String {
    "\(session.connected)|\(session.currentRunId ?? "")|\(session.runStatus.map { String(describing: $0.state) } ?? "")"
  }

  var body: some View {
    // Sidebar, title and toolbar come from the split view so they never overlap,
    // whatever the window size, sidebar width or project name.
    NavigationSplitView(columnVisibility: $workspace.columns) {
      if let tab = workspace.active {
        WorkbenchSidebar(workspace: workspace, session: session, tab: tab)
          .navigationSplitViewColumnWidth(min: 200, ideal: 250, max: 380)
      }
    } detail: {
      if EnvironmentOverlay.hasProblem(session) {
        // Nothing of the canvas stays behind the problem: it replaces the content.
        EnvironmentProblemView(session: session, onSwitchProject: onSwitchProject)
      } else {
      ChatSplit(width: chatWidth) {
        VStack(spacing: 0) {
          if workspace.tabs.count > 1 { tabBar }
          if let tab = workspace.active {
            Workbench(workspace: workspace, session: session, tab: tab, onSwitchProject: onSwitchProject)
              .id(tab.id)
              .focusedSceneValue(\.workflowTab, tab)
              .onChange(of: tab.scope) { session.saveViewSoon(scope: tab.scope, selection: tab.selection) }
              .onChange(of: tab.selection) { session.saveViewSoon(scope: tab.scope, selection: tab.selection) }
          }
        }
        if workspace.showChat {
          // Wider by default; the width the user gives it is kept (per Mac).
          // The chat is Tsugumi: the bird waits at its top left (GuideNest).
          ChatColumn(width: $chatWidth) {
            ChatPanel(session: session, chat: model.chat(for: session), detached: false, tab: workspace.active, shown: workspace.showChat)
          }
        }
      }
      }
    }
    .tsugumiLoading(EnvironmentOverlay.loading(session))
    .sheet(item: Binding(get: { model.chat(for: session).permissionPrompt }, set: { _ in })) { prompt in
      PermissionPromptView(prompt: prompt) { allow, always in
        model.chat(for: session).answerPermissions(allow: allow, always: always)
      }
    }
    // A demo tour: Tsugumi of this chat leads it, following the real runs.
    .onChange(of: runKey, initial: true) { if guideWindow != nil { GuideController.shared.observe(session, tab: workspace.active) } }
    .onChange(of: workspace.active?.id, initial: true) {
      if guideWindow != nil { GuideController.shared.setContext(.project, session: session, tab: workspace.active) }
    }
    // The window the user looks at now is the one a tapped choice is asked from.
    .onChange(of: activeState) {
      if guideWindow != nil, activeState == .key { GuideController.shared.setContext(.project, session: session, tab: workspace.active) }
    }
    .onAppear { if guideWindow != nil, GuideController.shared.touring(session) { workspace.showChat = true } }
    // Tsugumi points into a Card in Card (or back at a root Card): the canvas shows that workflow.
    .onChange(of: GuideController.shared.scopeRequest) { _, s in
      // Only the window of the project being talked about (the next, of another, has nothing to show).
      guard guideWindow != nil, let s, let tab = workspace.active, GuideController.shared.speakingChat?.session === session else { return }
      GuideController.shared.scopeRequest = nil
      if session.kind(key(s)) == .subgraph || s.isEmpty { tab.enter(s) }
    }
    .onChange(of: GuideController.shared.highlight) { _, h in
      guard guideWindow != nil, let h, h.hasPrefix("card:"), !h.dropFirst(5).contains("/"), let tab = workspace.active,
            !tab.scope.isEmpty, GuideController.shared.speakingChat?.session === session else { return }
      let id = String(h.dropFirst(5))
      if session.instance(key(tab.scope + [id])) == nil, session.instance(key([id])) != nil { tab.enter([]) }
    }
    .focusedSceneValue(\.projectSession, session)
    .focusedSceneValue(\.workspace, workspace)
    // The title bar stays quiet: the project's name and where the canvas is
    // float on the canvas (CanvasWorkspace); the window keeps its title for
    // the Window menu and Mission Control.
    .navigationTitle(session.scopeName([]))
    .toolbar(removing: .title)
    .toolbarBackgroundVisibility(.hidden, for: .windowToolbar)
  }

  private var tabBar: some View {
    ScrollView(.horizontal, showsIndicators: false) {
      HStack(spacing: 2) {
        ForEach(workspace.tabs) { t in
          let active = t.id == workspace.active?.id
          HStack(spacing: 6) {
            Image(systemName: t.scope.isEmpty ? "square.stack.3d.up.fill" : "square.stack.3d.up")
              .font(.caption)
            Text(session.scopeName(t.scope)).lineLimit(1)
            if workspace.tabs.count > 1 {
              Button { workspace.closeTab(t.id) } label: { Image(systemName: "xmark").font(.caption2) }
                .buttonStyle(.borderless)
                .help("タブを閉じる")
            }
          }
          .padding(.horizontal, 10).padding(.vertical, 5)
          .background(active ? Color.accentColor.opacity(0.18) : Color.clear, in: RoundedRectangle(cornerRadius: 6))
          .contentShape(Rectangle())
          .onTapGesture { workspace.activeTab = t.id }
          .accessibilityAddTraits(active ? [.isSelected, .isButton] : .isButton)
        }
      }
      .padding(.horizontal, 8).padding(.vertical, 4)
    }
    .background(.bar)
    .overlay(alignment: .bottom) { Divider() }
  }
}

/// Canvas and the status/log area of one tab (the sidebar and the chat
/// belong to the window's split view).
struct Workbench: View {
  @Bindable var workspace: Workspace
  let session: ProjectSession
  @Bindable var tab: WorkflowTab
  var onSwitchProject: (() -> Void)? = nil

  var body: some View {
    VStack(spacing: 0) {
      if workspace.showBottom {
        VSplitView {
          CanvasWorkspace(workspace: workspace, session: session, tab: tab, onSwitchProject: onSwitchProject)
            .frame(minHeight: 240)
          LogTimelinePanel(workspace: workspace, session: session, tab: tab)
            .frame(minHeight: 140, idealHeight: 220)
        }
      } else {
        CanvasWorkspace(workspace: workspace, session: session, tab: tab, onSwitchProject: onSwitchProject)
          .frame(minHeight: 240, maxHeight: .infinity)
      }
    }
    .frame(minWidth: 360)
  }
}

/// The workbench with the chat at its right. Not `.inspector`: on macOS 27.0 a
/// NavigationSplitView with an inspector can end the app with AppKit's "more
/// Update Constraints in Window passes than there are views" (it did on opening
/// a project on 27.0.1; https://developer.apple.com/forums/thread/836901). So
/// the window's split view only ever sees the workbench's size: the chat takes
/// its width from what is there and asks for nothing.
struct ChatSplit: Layout {
  static let minChat = 320.0, maxChat = 900.0, minWorkbench = 360.0
  var width: Double

  /// The chat's width in `available`: the workbench keeps its minimum.
  static func chatWidth(_ wanted: Double, in available: Double) -> Double {
    min(max(wanted, minChat), maxChat, max(0, available - minWorkbench))
  }

  func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
    subviews.first?.sizeThatFits(proposal) ?? .zero
  }

  func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
    let chat = subviews.count > 1 ? Self.chatWidth(width, in: bounds.width) : 0
    subviews.first?.place(at: bounds.origin, proposal: ProposedViewSize(width: bounds.width - chat, height: bounds.height))
    if subviews.count > 1 {
      subviews[1].place(at: CGPoint(x: bounds.maxX - chat, y: bounds.minY), proposal: ProposedViewSize(width: chat, height: bounds.height))
    }
  }
}

/// The chat's column: its left edge is dragged to set the width.
private struct ChatColumn<Content: View>: View {
  @Binding var width: Double
  @ViewBuilder var content: Content
  @State private var shown = 0.0
  @State private var dragStart: Double?

  var body: some View {
    content
      .clipped()
      .onGeometryChange(for: Double.self) { $0.size.width } action: { shown = $0 }
      .overlay(alignment: .leading) {
        Color.clear
          .frame(width: 9)
          .overlay { Rectangle().fill(Color(nsColor: .separatorColor)).frame(width: 1) }
          .contentShape(Rectangle())
          .pointerStyle(.columnResize)
          .offset(x: -4)
          .gesture(DragGesture(minimumDistance: 1, coordinateSpace: .global)
            .onChanged { v in
              let start = dragStart ?? shown
              dragStart = start
              width = min(max((start - v.translation.width).rounded(), ChatSplit.minChat), ChatSplit.maxChat)
            }
            .onEnded { _ in dragStart = nil })
      }
  }
}

/// What the window shows instead of the workbench while the environment is
/// not usable: the loading card while it starts and connects (`loading(_:)`),
/// the problem page when it stopped or failed (`hasProblem(_:)`).
@MainActor
enum EnvironmentOverlay {

  /// What the loading card says while the environment starts or the project
  /// opens; nil once connected or when there is a problem to show instead.
  static func loading(_ session: ProjectSession) -> LoadingContent? {
    guard !session.connected else { return nil }
    let dirty = session.dirty
    switch session.environment {
    case .failed, .stopped, .incompatibleHistory:
      return nil
    case .reconnecting:
      return LoadingContent(title: String(localized: "再接続しています…"),
                            message: String(localized: "未保存の変更は保持しています。"), dirty: dirty)
    case .preparing(let s):
      return LoadingContent(title: String(localized: "初回の準備をしています"), message: stageText(s), fraction: stageFraction(s),
                            detail: s, dirty: dirty, showsConsole: true)
    case .starting(let s):
      return LoadingContent(title: String(localized: "起動しています"), message: stageText(s), fraction: stageFraction(s),
                            detail: s, dirty: dirty, showsConsole: true)
    case .ready:
      if session.connectionMessage != nil { return nil }
      return LoadingContent(title: String(localized: "プロジェクトを読み込んでいます…"), detail: "project snapshot", dirty: dirty)
    }
  }

  /// The step in words; the internal name stays in the details line.
  static func stageText(_ raw: String) -> String {
    switch EnvironmentStage(rawValue: raw) {
    case .vmImage?: return String(localized: "VM イメージを展開しています")
    case .vmDisk?: return String(localized: "Nix store の保存領域を作っています")
    case .vmBoot?: return String(localized: "VM を起動しています")
    case .vmStore?: return String(localized: "Nix store の保存領域を開いています（初回は初期化）")
    case .vmNix?: return String(localized: "Nix を準備しています")
    case .vmNetwork?: return String(localized: "ネットワークを設定しています")
    case .vmSystem?: return String(localized: "VM の中で Tsugumi を組み立てています（初回とアプリの更新後のみ。1〜2 分）")
    case .vmSupervisor?: return String(localized: "実行環境の管理プロセスを起動しています")
    case .startingDaemon?: return String(localized: "このプロジェクトの tsugumid を起動しています")
    case .connecting?: return String(localized: "tsugumid の応答を待っています")
    case nil: return raw
    }
  }

  /// How far along the start is: the steps are known, so the branch grows.
  static func stageFraction(_ raw: String) -> Double? {
    guard let s = EnvironmentStage(rawValue: raw), let i = EnvironmentStage.allCases.firstIndex(of: s) else { return nil }
    return Double(i) / Double(EnvironmentStage.allCases.count)
  }

  /// Whether the environment or the project cannot be used now (a problem
  /// is shown instead of the canvas).
  static func hasProblem(_ s: ProjectSession) -> Bool {
    guard !s.connected else { return false }
    switch s.environment {
    case .failed, .stopped, .incompatibleHistory: return true
    case .ready: return s.connectionMessage != nil
    case .reconnecting, .preparing, .starting: return false
    }
  }
}

/// The page shown when the environment did not start (or stopped, or the
/// project could not be read): what happened, what to do, the actions, and
/// the log for the details. It replaces the canvas, on the window background.
struct EnvironmentProblemView: View {
  let session: ProjectSession
  var onSwitchProject: (() -> Void)?
  @State private var showLog = false
  @State private var copied = false

  private struct Problem {
    var symbol: String
    var color: Color
    var title: String
    var message: String
    var recovery: String?
    var log: String
    var canStop = true
    var historyArchiveAction: String?
    /// The line of the log that says why (tsugumid's last error).
    var cause: String?
  }

  /// The last error line of tsugumid's log, without its time and level.
  static func cause(in log: String) -> String? {
    guard let line = log.split(separator: "\n").last(where: { $0.contains(" error ") }) else { return nil }
    if let r = line.range(of: " error ") { return String(line[r.upperBound...]).trimmingCharacters(in: .whitespaces) }
    return String(line)
  }

  private var problem: Problem {
    switch session.environment {
    case .incompatibleHistory(let issue):
      let error = LauncherError.incompatibleHistory(issue)
      let title = issue.archiveInterrupted ? error.description : String(localized: "実行履歴の形式が異なります")
      let action = issue.archiveInterrupted ? String(localized: "履歴の退避を再開して起動") : String(localized: "履歴を退避して起動")
      return Problem(symbol: "clock.arrow.circlepath", color: .orange, title: title,
                     message: issue.archiveInterrupted ? "" : error.description, recovery: error.recovery, log: "", historyArchiveAction: action)
    case .failed(let m, let recovery, let log):
      var p = Problem(symbol: "xmark.octagon.fill", color: .red, title: String(localized: "実行環境を起動できませんでした"),
                      message: m, recovery: recovery.isEmpty ? nil : recovery, log: log)
      p.cause = Self.cause(in: log)
      if let c = p.cause, c.contains("cannot open the project") {
        p.recovery = String(localized: "プロジェクトのファイル（tsugumi.json・layout.json・definitions の中）の書式を直してから、再試行してください。文書と実行履歴は消えません。")
      }
      return p
    case .stopped:
      return Problem(symbol: "pause.circle.fill", color: .secondary, title: String(localized: "実行環境は停止しています"),
                     message: String(localized: "このプロジェクトの実行環境（tsugumid）は動いていません。起動すると編集と実行ができます。"),
                     recovery: nil, log: "", canStop: false)
    default:
      if session.connectionFailed {
        return Problem(symbol: "exclamationmark.triangle.fill", color: .orange, title: String(localized: "実行環境に接続できませんでした"),
                       message: session.connectionMessage ?? "",
                       recovery: String(localized: "再試行してください。"), log: "")
      }
      return Problem(symbol: "exclamationmark.triangle.fill", color: .orange, title: String(localized: "プロジェクトを読み込めませんでした"),
                     message: session.connectionMessage ?? "",
                     recovery: String(localized: "tsugumi.json などの書式が正しいかを確かめて、もう一度試してください。"), log: "")
    }
  }

  var body: some View {
    let p = problem
    ScrollView {
      VStack(alignment: .leading, spacing: 18) {
        HStack(alignment: .center, spacing: 14) {
          Image(systemName: p.symbol).font(.system(size: 34)).foregroundStyle(p.color)
          VStack(alignment: .leading, spacing: 3) {
            Text(p.title).font(.title2.bold())
            Text(session.scopeName([]) + " · " + session.identity.url.path)
              .font(.callout).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
          }
        }
        if !p.message.isEmpty {
          section("何が起きたか") {
            VStack(alignment: .leading, spacing: 8) {
              Text(p.message).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
              if let c = p.cause {
                VStack(alignment: .leading, spacing: 3) {
                  Text("原因（ログより）").font(.caption).foregroundStyle(.secondary)
                  Text(c).font(.callout.monospaced()).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
                }
              }
            }
          }
        }
        if let r = p.recovery {
          section("どうすればよいか") {
            Text(r).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
          }
        }
        if session.dirty {
          Label("保存できていない変更は保持しています（起動すると保存します）", systemImage: "pencil.circle")
            .font(.callout).foregroundStyle(.orange)
        }
        HStack(spacing: 10) {
          Button(p.canStop ? "再試行" : "起動") { session.start() }
            .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
          if let action = p.historyArchiveAction {
            Button(action) { session.archiveHistoryAndStart() }
          } else if p.canStop {
            Button("環境を停止") { session.stopEnvironment() }
          }
          OpenConsoleButton()
          Spacer()
          if let onSwitchProject { Button("別のプロジェクトを開く…", action: onSwitchProject) }
        }
        if !p.log.isEmpty {
          DisclosureGroup(isExpanded: $showLog) {
            VStack(alignment: .trailing, spacing: 6) {
              ScrollView {
                Text(String(p.log.suffix(20000))).font(.caption.monospaced()).textSelection(.enabled)
                  .frame(maxWidth: .infinity, alignment: .leading).padding(8)
              }
              .frame(height: 220)
              .background(Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 7))
              .overlay(RoundedRectangle(cornerRadius: 7).strokeBorder(Color.secondary.opacity(0.3)))
              Button(copied ? "コピーしました" : "ログをコピー") {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(p.log, forType: .string)
                copied = true
              }
              .font(.caption)
            }
            .padding(.top, 6)
          } label: {
            Text("詳しいログ").font(.callout.weight(.semibold))
          }
        }
      }
      .padding(32)
      .frame(maxWidth: 680, alignment: .leading)
      .frame(maxWidth: .infinity)
    }
    .background(Color(nsColor: .windowBackgroundColor))
    .ignoresSafeArea(.container, edges: .top)
    .safeAreaPadding(.top, 30)
  }

  private func section(_ title: LocalizedStringKey, @ViewBuilder _ content: () -> some View) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      Text(title).font(.callout.weight(.semibold)).foregroundStyle(.secondary)
      content()
    }
    .padding(14)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(Color.secondary.opacity(0.07), in: RoundedRectangle(cornerRadius: 10))
  }
}

/// Run entry points shared by the canvas control, menus and shortcuts.
@MainActor
enum RunActions {
  /// The whole document runs from its root trigger, whichever scope is shown.
  static func runAll(_ s: ProjectSession, scope: [String]) {
    let trigger = s.cards(in: []).first { c in
      c["id"]?.string.map { s.kind(key([$0])) } == .trigger
    }
    guard let id = trigger?["id"]?.string else {
      s.notice = String(localized: "全体を実行するには最上位の Workflow に開始 Card（trigger）を置いてください。Card を選んで個別に実行もできます。")
      return
    }
    Task {
      if s.computationDirty { await s.save() }
      do { try await s.run(card: [id], propagate: true) } catch { s.notice = "\(error)" }
    }
  }

  static func run(_ s: ProjectSession, card: CardKey, propagate: Bool) {
    Task {
      if s.computationDirty { await s.save() }
      do { try await s.run(card: path(card), propagate: propagate) } catch { s.notice = "\(error)" }
    }
  }
}

/// Human-readable run and Card states with an icon (never color alone).
extension Tsugumi_Data_RunState {
  var label: String {
    switch self {
    case .idle: return String(localized: "待機中")
    case .queued: return String(localized: "待機列")
    case .preparing: return String(localized: "準備中")
    case .running: return String(localized: "実行中")
    case .cancelling: return String(localized: "停止中")
    case .succeeded: return String(localized: "成功")
    case .failed: return String(localized: "失敗")
    case .cancelled: return String(localized: "停止済み")
    default: return String(localized: "未実行")
    }
  }

  var symbol: String {
    switch self {
    case .queued, .preparing: return "hourglass"
    case .running: return "play.circle.fill"
    case .cancelling: return "stop.circle"
    case .succeeded: return "checkmark.circle.fill"
    case .failed: return "xmark.circle.fill"
    case .cancelled: return "stop.circle.fill"
    default: return "circle"
    }
  }

  var color: Color {
    switch self {
    case .running, .preparing, .queued, .cancelling: return .orange
    case .succeeded: return .green
    case .failed: return .red
    case .cancelled: return .brown
    default: return .secondary
    }
  }
}

extension Tsugumi_Data_CardState {
  var label: String {
    switch self {
    case .notStarted: return String(localized: "未実行")
    case .waiting: return String(localized: "入力待ち")
    case .ready: return String(localized: "実行待ち")
    case .preparing: return String(localized: "準備中")
    case .running: return String(localized: "実行中")
    case .succeeded: return String(localized: "成功")
    case .failed: return String(localized: "失敗")
    case .skipped: return String(localized: "スキップ")
    case .blocked: return String(localized: "ブロック")
    case .cancelled: return String(localized: "停止済み")
    default: return String(localized: "不明")
    }
  }

  var symbol: String {
    switch self {
    case .waiting, .ready: return "hourglass"
    case .preparing: return "gearshape"
    case .running: return "play.circle.fill"
    case .succeeded: return "checkmark.circle.fill"
    case .failed: return "xmark.circle.fill"
    case .skipped: return "arrow.uturn.right.circle"
    case .blocked: return "nosign"
    case .cancelled: return "stop.circle.fill"
    default: return "circle.dotted"
    }
  }

  var color: Color {
    switch self {
    case .running, .preparing, .waiting, .ready: return .orange
    case .succeeded: return .green
    case .failed: return .red
    case .blocked: return .purple
    case .skipped: return .gray
    case .cancelled: return .brown
    default: return .secondary
    }
  }
}

func formatMillis(_ ms: Int64?) -> String {
  guard let ms else { return "" }
  return Date(timeIntervalSince1970: Double(ms) / 1000).formatted(date: .abbreviated, time: .standard)
}

func formatDuration(_ seconds: Double) -> String {
  // One form everywhere (run, stages, timeline, history): hh:mm:ss.xx.
  let cs = Int((max(0, seconds) * 100).rounded(.down))
  return String(format: "%02d:%02d:%02d.%02d", cs / 360_000, cs / 6000 % 60, cs / 100 % 60, cs % 100)
}
