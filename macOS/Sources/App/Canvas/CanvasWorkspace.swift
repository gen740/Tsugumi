import AppKit
import SwiftUI
import TsugumiRPC

/// The canvas with its floating controls. The canvas reaches the top of the
/// window; the project's title (with where the canvas is), the run control
/// and the project's actions float on it with the same look.
struct CanvasWorkspace: View {
  @Environment(AppModel.self) private var model
  @Environment(AppPreferences.self) private var prefs
  @Bindable var workspace: Workspace
  let session: ProjectSession
  @Bindable var tab: WorkflowTab
  var onSwitchProject: (() -> Void)? = nil
  /// Where the window buttons and the sidebar button end (title bar).
  static let besideSidebarButton: CGFloat = 142

  var body: some View {
    ZStack(alignment: .top) {
      GraphCanvas(workspace: workspace, session: session, tab: tab)
        .ignoresSafeArea(.container, edges: .top)
      // The band along the top of the window moves the window (as a title
      // bar does; a double click zooms it). The canvas starts below it, so
      // the two never compete for a drag.
      WindowDragBand()
        .frame(height: 34)
        .ignoresSafeArea(.container, edges: .top)
    }
    .overlay(alignment: .top) {
      // One row so the controls never overlap; the run control stays centered when there is room.
      HStack(alignment: .top, spacing: 8) {
        // The title keeps its room; only very long names are cut.
        breadcrumbs.layoutPriority(3)
        Spacer(minLength: 0)
        CanvasRunControl(workspace: workspace, session: session, tab: tab).fixedSize().layoutPriority(2)
        Spacer(minLength: 0)
        CanvasDraftControls(workspace: workspace, session: session, tab: tab).fixedSize().layoutPriority(1)
        CanvasCommentControls(workspace: workspace, session: session, tab: tab).fixedSize().layoutPriority(1)
        projectActions.fixedSize().layoutPriority(2)
      }
      // In the title bar's row, beside the window buttons: right of the
      // sidebar button while the sidebar is closed.
      .padding(.leading, workspace.columns == .detailOnly ? Self.besideSidebarButton : 10)
      .padding(.trailing, 10)
      .padding(.top, 11)
      .ignoresSafeArea(.container, edges: .top)
    }
    // Zoom and layout controls, and the run and save state with counts and
    // the panels: floating like the controls at the top, in one row so they
    // never overlap (the state takes a shorter form when room is short).
    .overlay(alignment: .bottom) {
      HStack(alignment: .bottom, spacing: 8) {
        viewControls.fixedSize()
        Spacer(minLength: 0)
        StatusBar(workspace: workspace, session: session, tab: tab)
      }
      .padding(10)
    }
  }

  /// Back to Home (as the menu does, with its refusal while a run is on), chat:
  /// beside the run control, the same look.
  private var projectActions: some View {
    HStack(spacing: 10) {
      if let onSwitchProject {
        Button(action: onSwitchProject) { Image(systemName: "house") }
          .help(tip(String(localized: "Home へ戻る"), .home))
          .accessibilityLabel("Home へ戻る")
      }
      Button { workspace.showChat.toggle() } label: {
        // While the chat is closed, Tsugumi waits on its button.
        if !workspace.showChat && onSwitchProject != nil && GuideController.shared.available {
          GuideNest(busy: AppModel.shared.chat(for: session).busy, size: 22)
        } else {
          Image(systemName: workspace.showChat ? "bubble.left.and.text.bubble.right.fill" : "bubble.left.and.text.bubble.right")
        }
      }
      .help(workspace.showChat ? "チャットを閉じる（\(AppPreferences.shared.shortcut(.toggleChat).label)）"
                               : "チャットを開く（\(AppPreferences.shared.shortcut(.toggleChat).label)）")
      .accessibilityLabel("チャット")
      .guideTarget("chat")
    }
    .buttonStyle(.borderless)
    .font(.callout)
    .padding(.horizontal, 12).padding(.vertical, 7)
    .background(.regularMaterial, in: Capsule())
  }

  /// The project's title and where the canvas is (Card in Card), in one place.
  private var breadcrumbs: some View {
    HStack(spacing: 4) {
      if !(tab.scope.isEmpty && tab.back.isEmpty) {
        Button { goUp() } label: { Image(systemName: "chevron.backward") }
          .buttonStyle(.borderless)
          .help("戻る")
      }
      Button { tab.enter([]) } label: {
        // The whole name when it fits; cut in the middle only when it does not.
        ViewThatFits(in: .horizontal) {
          Text(session.scopeName([])).fontWeight(.semibold).fixedSize()
          Text(session.scopeName([])).fontWeight(.semibold).lineLimit(1).truncationMode(.middle)
        }
      }
        .buttonStyle(.plain)
        .help(session.identity.url.path)
      ForEach(Array(tab.scope.enumerated()), id: \.offset) { i, _ in
        Image(systemName: "chevron.right").font(.caption2).foregroundStyle(.secondary)
        Button { tab.enter(Array(tab.scope.prefix(i + 1))) } label: {
          Text(session.scopeName(Array(tab.scope.prefix(i + 1)))).lineLimit(1).truncationMode(.middle)
        }
        .buttonStyle(.link)
      }
    }
    .frame(maxWidth: 360, alignment: .leading)
    .fixedSize(horizontal: false, vertical: true)
    .font(.callout)
    .padding(.horizontal, 12).padding(.vertical, 7)
    .background(.regularMaterial, in: Capsule())
  }

  private func goUp() {
    if !tab.back.isEmpty { tab.goBack() } else if !tab.scope.isEmpty { tab.enter(Array(tab.scope.dropLast())) }
  }

  private var viewControls: some View {
    let zoom = session.cameras[key(tab.scope)]?.zoom ?? 1
    return HStack(spacing: 2) {
      Button { setZoom(zoom / 1.2) } label: { Image(systemName: "minus.magnifyingglass") }.help(tip(String(localized: "縮小"), .zoomOut))
      Button { setZoom(1) } label: { Text("\(Int((zoom * 100).rounded()))%").monospacedDigit().frame(minWidth: 42) }.help(tip("100% に戻す", .actualSize))
      Button { setZoom(zoom * 1.2) } label: { Image(systemName: "plus.magnifyingglass") }.help(tip(String(localized: "拡大"), .zoomIn))
      Divider().frame(height: 16)
      Button { NotificationCenter.default.post(name: .canvasFitAll, object: tab.id) } label: { Image(systemName: "arrow.up.left.and.down.right.magnifyingglass") }
        .help(tip(String(localized: "全体表示"), .fitAll))
      // Notes, groups and folding are in the canvas's context menu and the
      // menu bar, where they are named; small icons for them read as nothing.
      Button { session.arrange(tab.scope) } label: { Image(systemName: "rectangle.grid.2x2") }
        .help("整列（配置だけを変更）")
      Divider().frame(height: 16)
      Toggle(isOn: Binding(get: { prefs.showGrid }, set: { prefs.showGrid = $0 })) { Image(systemName: "grid") }
        .toggleStyle(.button)
        .help("グリッド")
    }
    .buttonStyle(.borderless)
    .padding(.horizontal, 10).padding(.vertical, 5)
    .background(.regularMaterial, in: Capsule())
  }

  private func setZoom(_ z: Double) {
    var c = session.cameras[key(tab.scope)] ?? (40, 40, 1)
    let nz = max(0.2, min(3, z))
    c.x = c.x * nz / c.zoom
    c.y = c.y * nz / c.zoom
    c.zoom = nz
    session.cameras[key(tab.scope)] = c
    session.saveViewSoon(scope: tab.scope, selection: tab.selection)
  }
}

/// Run / stop, the run's state and what it is doing now, elapsed time and
/// the issue list. Per-Card counts and details are on the Cards, in the
/// history and on the timeline (a share of finished Cards says nothing about
/// a run with loops, so there is no progress bar).
struct CanvasRunControl: View {
  @Bindable var workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab
  @State private var showIssues = false
  /// Shown as running: from the start, and for a moment after a quick run,
  /// so a short run reads as one step instead of a flicker.
  @State private var shownActive = false
  @State private var activeSince = Date.distantPast

  var body: some View {
    let state = session.runStatus?.state ?? .idle
    let problems = session.diagnostics.filter(\.error).count + session.cardStates.values.filter { $0.state == .failed }.count
    HStack(spacing: 8) {
      // One button of a fixed size: its title and color change, nothing moves.
      Button {
        if session.runActive { Task { await session.cancelRun() } } else { RunActions.runAll(session, scope: tab.scope) }
      } label: {
        Label(shownActive ? "停止" : "実行", systemImage: shownActive ? "stop.fill" : "play.fill")
          .frame(minWidth: 52)
      }
      .buttonStyle(.borderedProminent)
      .tint(shownActive ? .red : .accentColor)
      .guideTarget("run")
      .disabled(shownActive ? (!session.runActive || state == .cancelling) : !session.connected)
      .help(shownActive ? tip(String(localized: "Workflow の実行を停止（チャットの回答停止とは別の操作です）"), .stop)
                        : tip(String(localized: "文書の Workflow 全体を開始 Card から実行（未保存の変更は先に保存します）"), .run))
      // State and time on one line in a fixed width (what runs now is in the
      // tooltip); the clock ticks in hundredths while the run is active.
      HStack(spacing: 4) {
        Image(systemName: shownActive ? Tsugumi_Data_RunState.running.symbol : state.symbol)
          .foregroundStyle(shownActive ? Tsugumi_Data_RunState.running.color : state.color)
        Text(shownActive && !session.runActive ? Tsugumi_Data_RunState.running.label : state.label).lineLimit(1)
        SwiftUI.TimelineView(.periodic(from: .now, by: session.runActive ? 0.03 : 1)) { _ in
          Text(elapsed).monospacedDigit().foregroundStyle(.secondary).fixedSize()
        }
      }
      .frame(width: 172, alignment: .leading)
      .help(shownActive ? (activity ?? state.label) + String(localized: "（経過時間は Run の受付から、実行環境の時計で計ります）")
                        : String(localized: "Run の状態と経過時間"))
      .accessibilityElement(children: .combine)
      Button { showIssues.toggle() } label: {
        HStack(spacing: 2) {
          Image(systemName: problems > 0 ? "exclamationmark.triangle.fill" : "checkmark.seal")
          Text("\(problems)").monospacedDigit()
        }
        .foregroundStyle(problems > 0 ? .orange : .secondary)
      }
      .buttonStyle(.borderless)
      .help(String(localized: "問題 \(problems) 件（一覧から対象の Card やログへ移動できます）"))
      .popover(isPresented: $showIssues) {
        IssueList(workspace: workspace, session: session, tab: tab).frame(width: 460, height: 320)
      }
      Divider().frame(height: 14)
      Menu {
        BuiltinCardItems { t in NotificationCenter.default.post(name: .canvasAddBuiltin, object: tab.id, userInfo: ["template": t.rawValue]) }
      } label: { Image(systemName: "plus") }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
        .disabled(!session.isEditable)
        .help("組み込みの Card を追加（入力値・Draft・開始・分配・分岐・合流・記録・処理）")
        .accessibilityLabel("Card を追加")
    }
    .font(.callout)
    .padding(.horizontal, 8).padding(.vertical, 5)
    .background(.regularMaterial, in: Capsule())
    .onChange(of: session.runActive, initial: true) { _, active in
      if active {
        shownActive = true
        activeSince = Date()
      } else {
        let wait = max(0, 0.8 - Date().timeIntervalSince(activeSince))
        Task {
          try? await Task.sleep(for: .seconds(wait))
          if !session.runActive { shownActive = false }
        }
      }
    }
  }

  /// What the run is doing now: the stage of a running Card, or how many run.
  private var activity: String? {
    let running = session.cardStates.filter { [.running, .preparing].contains($0.value.state) }
    if running.count == 1, let (k, s) = running.first {
      return session.displayName(k) + " " + (s.stage?.activeLabel ?? s.state.label)
    }
    if running.count > 1 { return String(localized: "\(running.count) 枚が実行中") }
    if session.runStatus?.state == .preparing { return String(localized: "実行環境を準備中") }
    return nil
  }

  /// Run time on the core's clock from acceptance: the same measure while
  /// it runs and once it ended (the timeline uses the same origin).
  private var elapsed: String {
    guard let s = session.runStatus, let start = s.acceptedAt ?? s.startedAt else { return "" }
    let end = s.endedAt ?? (session.runActive ? session.coreNow() : start)
    return formatDuration(Double(max(0, end - start)) / 1000)
  }
}

/// Shown when the current scope has Draft Cards: generate them with the chat.
/// Shown when Cards of the current scope have open comments: the selected ones
/// (or all of them) go to the chat.
struct CanvasCommentControls: View {
  @Environment(AppModel.self) private var model
  @Bindable var workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab

  var body: some View {
    let cards = session.cardsWithOpenComments(in: tab.scope)
    if !cards.isEmpty {
      let chosen = cards.filter { tab.selection.contains($0) }
      let asked = chosen.isEmpty ? cards : chosen
      let count = asked.reduce(0) { $0 + session.openComments($1).count }
      Button {
        workspace.showChat = true
        model.chat(for: session).generateFromComments(scope: tab.scope, cards: asked)
      } label: { Label("コメントから生成（\(count)）", systemImage: "bubble.left") }
        .disabled(session.dirty || !session.connected || model.chat(for: session).busy)
        .help(session.dirty ? "先に変更を保存してください" :
                chosen.isEmpty ? "この階層の Card の未解決のコメントに従って、チャットで Card を書き換えます（コメントが求めれば Card を足します）。進捗はチャットに表示されます。"
                : "選択した Card の未解決のコメントに従って、チャットで Card を書き換えます（コメントが求めれば Card を足します）。進捗はチャットに表示されます。")
        .font(.callout)
        .padding(.horizontal, 10).padding(.vertical, 6)
        .background(.regularMaterial, in: Capsule())
    }
  }
}

struct CanvasDraftControls: View {
  @Environment(AppModel.self) private var model
  @Bindable var workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab

  var body: some View {
    let drafts = session.cards(in: tab.scope).compactMap { $0["id"]?.string }.map { key(tab.scope + [$0]) }
      .filter { session.kind($0) == .draft }
    if !drafts.isEmpty {
      Button {
        workspace.showChat = true
        model.chat(for: session).generateDrafts(scope: tab.scope, cards: drafts)
      } label: { Label("Card を生成", systemImage: "wand.and.stars") }
        .disabled(session.dirty || !session.connected || session.runActive || model.chat(for: session).busy)
        .help(session.dirty ? "先に変更を保存してください（配線と Draft の内容を確定してから生成します）" :
                "この階層の Draft をチャットで実装します。進捗はチャットに表示されます。")
        .font(.callout)
        .padding(.horizontal, 10).padding(.vertical, 6)
        .background(.regularMaterial, in: Capsule())
    }
  }
}

/// A tooltip that names the shortcut of its action.
@MainActor func tip(_ text: String, _ action: ShortcutAction) -> String {
  "\(text)（\(AppPreferences.shared.shortcut(action).label)）"
}

/// Moves the window when dragged and zooms it on a double click, like a
/// title bar (the canvas reaches the top of the window under it).
struct WindowDragBand: View {
  var body: some View {
    Color.clear
      .contentShape(Rectangle())
      .gesture(WindowDragGesture())
      .onTapGesture(count: 2) {
        guard let w = NSApp.keyWindow else { return }
        // The system setting for a title bar double click.
        switch UserDefaults.standard.string(forKey: "AppleActionOnDoubleClick") {
        case "Minimize": w.miniaturize(nil)
        case "None": break
        default: w.zoom(nil)
        }
      }
      .accessibilityHidden(true)
  }
}
