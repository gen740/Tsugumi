import AppKit
import SwiftUI
import TsugumiLauncher
import TsugumiRPC

/// Tabs of one project (the root workflow and opened child workflows) and the
/// workbench of the active tab.
struct WorkflowTabs: View {
  @Environment(AppModel.self) private var model
  @Bindable var workspace: Workspace
  let session: ProjectSession
  let onSwitchProject: (() -> Void)?

  var body: some View {
    VStack(spacing: 0) {
      if workspace.tabs.count > 1 { tabBar }
      if let tab = workspace.active {
        Workbench(workspace: workspace, session: session, tab: tab)
          .id(tab.id)
          .focusedSceneValue(\.workflowTab, tab)
          .onChange(of: tab.scope) { session.saveViewSoon(scope: tab.scope, selection: tab.selection) }
          .onChange(of: tab.selection) { session.saveViewSoon(scope: tab.scope, selection: tab.selection) }
      }
    }
    .focusedSceneValue(\.projectSession, session)
    .focusedSceneValue(\.workspace, workspace)
    .navigationTitle(session.scopeName([]))
    .navigationSubtitle(workspace.active.map { $0.scope.isEmpty ? "" : session.scopeName($0.scope) } ?? "")
    .toolbar { toolbar }
  }

  @ToolbarContentBuilder private var toolbar: some ToolbarContent {
    ToolbarItemGroup(placement: .primaryAction) {
      if let onSwitchProject {
        Button(action: onSwitchProject) { Label("開く", systemImage: "folder") }
          .help(session.runActive ? "実行中は切り替えられません" : "別のプロジェクトを開く（必要な保存の後に切り替えます）")
          .disabled(session.runActive)
      }
      Button { Task { await session.save() } } label: { Label("保存", systemImage: "square.and.arrow.down") }
        .disabled(!session.dirty || !session.connected || session.saving)
        .help(session.dirty ? "変更を保存（\(AppPreferences.shared.shortcut(.save).label)）" : "未保存の変更はありません")
      Toggle(isOn: $workspace.showChat) { Label("チャット", systemImage: "bubble.left.and.text.bubble.right") }
        .help("チャットの表示切替")
    }
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

/// Sidebar, canvas, status/log area and chat of one tab.
struct Workbench: View {
  @Environment(AppModel.self) private var model
  @Bindable var workspace: Workspace
  let session: ProjectSession
  @Bindable var tab: WorkflowTab

  var body: some View {
    HSplitView {
      WorkbenchSidebar(workspace: workspace, session: session, tab: tab)
        .frame(minWidth: 210, idealWidth: 250, maxWidth: 360)
      VStack(spacing: 0) {
        if workspace.showBottom {
          VSplitView {
            CanvasWorkspace(workspace: workspace, session: session, tab: tab)
              .frame(minHeight: 280)
            LogTimelinePanel(workspace: workspace, session: session, tab: tab)
              .frame(minHeight: 160, idealHeight: 240)
          }
        } else {
          CanvasWorkspace(workspace: workspace, session: session, tab: tab)
            .frame(minHeight: 280, maxHeight: .infinity)
        }
        StatusBar(workspace: workspace, session: session, tab: tab)
      }
      .frame(minWidth: 480)
      if workspace.showChat {
        ChatPanel(session: session, chat: model.chat(for: session), detached: false, tab: tab)
          .frame(minWidth: 300, idealWidth: 360, maxWidth: 560)
      }
    }
    .overlay { EnvironmentOverlay(session: session) }
    .tsugumiLoading(EnvironmentOverlay.loading(session))
  }
}

/// Covers the workbench when the environment is stopped or failed, with the
/// reason and a retry. Starting and connecting are shown by the loading card
/// (`loading(_:)`). Unsaved edits stay in the draft.
struct EnvironmentOverlay: View {
  let session: ProjectSession

  /// What the loading card says while the environment starts or the project
  /// opens; nil once connected or when there is a problem to show instead.
  static func loading(_ session: ProjectSession) -> LoadingContent? {
    guard !session.connected else { return nil }
    let dirty = session.dirty
    switch session.environment {
    case .failed, .stopped:
      return nil
    case .reconnecting:
      return LoadingContent(title: String(localized: "再接続しています…"),
                            message: String(localized: "未保存の変更は保持しています。"), dirty: dirty)
    case .preparing(let s):
      return LoadingContent(title: String(localized: "初回の準備をしています"), message: s, dirty: dirty)
    case .starting(let s):
      return LoadingContent(title: String(localized: "起動しています"), message: s, dirty: dirty)
    case .ready:
      if session.connectionMessage != nil { return nil }
      return LoadingContent(title: String(localized: "プロジェクトを読み込んでいます…"), dirty: dirty)
    }
  }

  var body: some View {
    if !session.connected, let problem {
      ZStack {
        Rectangle().fill(.ultraThinMaterial)
        VStack(spacing: 12) {
          problem
          if session.dirty {
            Label("未保存の変更があります（保持しています）", systemImage: "pencil.circle").font(.callout).foregroundStyle(.orange)
          }
        }
        .padding(28)
        .frame(maxWidth: 620)
      }
      .transition(.opacity)
    }
  }

  private var problem: AnyView? {
    switch session.environment {
    case .failed(let m, let recovery, let log):
      return AnyView(VStack(spacing: 12) {
        Image(systemName: "xmark.octagon.fill").font(.largeTitle).foregroundStyle(.red)
        Text("実行環境を起動できませんでした").font(.title2.bold())
        Text(m).multilineTextAlignment(.center)
        Text(recovery).font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center)
        if !log.isEmpty {
          ScrollView { Text(log.suffix(4000)).font(.caption.monospaced()).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }
            .frame(maxWidth: 560, maxHeight: 160)
            .padding(6).background(.background.secondary, in: RoundedRectangle(cornerRadius: 6))
        }
        HStack {
          Button("再試行") { session.start() }.buttonStyle(.borderedProminent)
          Button("環境を停止") { session.stopEnvironment() }
        }
      })
    case .stopped:
      return AnyView(VStack(spacing: 12) {
        Image(systemName: "pause.circle").font(.largeTitle).foregroundStyle(.secondary)
        Text("実行環境は停止しています").font(.title2.bold())
        Button("起動") { session.start() }.buttonStyle(.borderedProminent)
      })
    case .ready:
      guard let m = session.connectionMessage else { return nil }
      return AnyView(VStack(spacing: 12) {
        Image(systemName: "exclamationmark.triangle.fill").font(.largeTitle).foregroundStyle(.orange)
        Text("プロジェクトを読み込めませんでした").font(.title2.bold())
        Text(m).font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center).textSelection(.enabled)
        Button("再試行") { session.start() }.buttonStyle(.borderedProminent)
      })
    case .reconnecting, .preparing, .starting:
      return nil
    }
  }
}

extension EnvironmentState {
  var isPreparing: Bool { if case .preparing = self { return true }; return false }
}

/// Run entry points shared by the canvas control, menus and shortcuts.
@MainActor
enum RunActions {
  /// The whole document runs from its root trigger, whichever scope is shown.
  static func runAll(_ s: ProjectSession, scope: [String]) {
    let trigger = s.cards(in: []).first { c in
      c["definition"]?.string.flatMap { s.definitions[$0]?["template"]?.string } == "trigger"
    }
    guard let id = trigger?["id"]?.string else {
      s.notice = "全体を実行するには最上位の Workflow に開始 Card（trigger）を置いてください。Card を選んで個別に実行もできます。"
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
  if seconds < 60 { return String(format: "%.1f 秒", seconds) }
  let m = Int(seconds) / 60, s = Int(seconds) % 60
  return m < 60 ? "\(m) 分 \(s) 秒" : "\(m / 60) 時間 \(m % 60) 分"
}
