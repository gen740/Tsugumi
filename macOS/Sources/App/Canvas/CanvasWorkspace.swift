import AppKit
import SwiftUI
import TsugumiRPC

/// The canvas with its floating controls.
struct CanvasWorkspace: View {
  @Environment(AppModel.self) private var model
  @Environment(AppPreferences.self) private var prefs
  @Bindable var workspace: Workspace
  let session: ProjectSession
  @Bindable var tab: WorkflowTab

  var body: some View {
    GraphCanvas(workspace: workspace, session: session, tab: tab)
      .overlay(alignment: .top) {
        // One row so the controls never overlap; the run control stays centered when there is room.
        HStack(alignment: .top, spacing: 8) {
          breadcrumbs.layoutPriority(0)
          Spacer(minLength: 0)
          CanvasRunControl(workspace: workspace, session: session, tab: tab).fixedSize().layoutPriority(2)
          Spacer(minLength: 0)
          CanvasDraftControls(workspace: workspace, session: session, tab: tab).fixedSize().layoutPriority(1)
        }
        .padding(10)
      }
      .overlay(alignment: .bottomLeading) { viewControls.padding(10) }
  }

  private var breadcrumbs: some View {
    HStack(spacing: 4) {
      Button { goUp() } label: { Image(systemName: "chevron.backward") }
        .buttonStyle(.borderless)
        .disabled(tab.scope.isEmpty && tab.back.isEmpty)
        .help("戻る")
      Button { tab.enter([]) } label: { Text(session.scopeName([])).lineLimit(1).truncationMode(.middle) }
        .buttonStyle(.link)
        .help(session.scopeName([]))
      ForEach(Array(tab.scope.enumerated()), id: \.offset) { i, _ in
        Image(systemName: "chevron.right").font(.caption2).foregroundStyle(.secondary)
        Button { tab.enter(Array(tab.scope.prefix(i + 1))) } label: {
          Text(session.scopeName(Array(tab.scope.prefix(i + 1)))).lineLimit(1).truncationMode(.middle)
        }
        .buttonStyle(.link)
      }
    }
    .frame(maxWidth: 320, alignment: .leading)
    .fixedSize(horizontal: false, vertical: true)
    .padding(.horizontal, 8).padding(.vertical, 5)
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
  }

  private func goUp() {
    if !tab.back.isEmpty { tab.goBack() } else if !tab.scope.isEmpty { tab.enter(Array(tab.scope.dropLast())) }
  }

  private var viewControls: some View {
    let zoom = session.cameras[key(tab.scope)]?.zoom ?? 1
    return HStack(spacing: 2) {
      Button { setZoom(zoom / 1.2) } label: { Image(systemName: "minus.magnifyingglass") }.help("縮小")
      Button { setZoom(1) } label: { Text("\(Int((zoom * 100).rounded()))%").monospacedDigit().frame(minWidth: 42) }.help("100% に戻す")
      Button { setZoom(zoom * 1.2) } label: { Image(systemName: "plus.magnifyingglass") }.help("拡大")
      Divider().frame(height: 16)
      Button { NotificationCenter.default.post(name: .canvasFitAll, object: tab.id) } label: { Image(systemName: "arrow.up.left.and.down.right.magnifyingglass") }
        .help("全体表示")
      Button { session.arrange(tab.scope) } label: { Image(systemName: "rectangle.grid.2x2") }
        .help("整列（配置だけを変更）")
      Button { NotificationCenter.default.post(name: .canvasAddNote, object: tab.id) } label: { Image(systemName: "note.text.badge.plus") }
        .help("付箋を追加")
      Button { _ = session.addGroup(tab.selection, in: tab.scope) } label: { Image(systemName: "rectangle.3.group") }
        .disabled(tab.selection.isEmpty)
        .help("選択した Card をグループ化")
      Button { session.setCollapsed(Set(tab.selection), !(tab.selection.allSatisfy { session.collapsed($0) })) } label: {
        Image(systemName: "rectangle.compress.vertical")
      }
      .disabled(tab.selection.isEmpty)
      .help("選択した Card を折りたたむ／展開（実行には影響しません）")
      Divider().frame(height: 16)
      Toggle(isOn: Binding(get: { prefs.showGrid }, set: { prefs.showGrid = $0 })) { Image(systemName: "grid") }
        .toggleStyle(.button)
        .help("グリッド")
    }
    .buttonStyle(.borderless)
    .padding(.horizontal, 8).padding(.vertical, 5)
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
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

  var body: some View {
    let state = session.runStatus?.state ?? .idle
    let problems = session.diagnostics.filter(\.error).count + session.cardStates.values.filter { $0.state == .failed }.count
    HStack(spacing: 10) {
      if session.runActive {
        Button { Task { await session.cancelRun() } } label: { Label("停止", systemImage: "stop.fill") }
          .buttonStyle(.borderedProminent).tint(.red)
          .disabled(state == .cancelling)
          .help("Workflow の実行を停止（チャットの回答停止とは別の操作です）")
      } else {
        Button { RunActions.runAll(session, scope: tab.scope) } label: { Label("実行", systemImage: "play.fill") }
          .buttonStyle(.borderedProminent)
          .disabled(!session.connected)
          .help("文書の Workflow 全体を開始 Card から実行（未保存の変更は先に保存します）")
        Menu {
          Button("選択した Card から下流を実行") { if let k = tab.selection.first { RunActions.run(session, card: k, propagate: true) } }
          Button("選択した Card だけ実行（最後の入力）") { if let k = tab.selection.first { RunActions.run(session, card: k, propagate: false) } }
        } label: { Image(systemName: "chevron.down") }
          .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
          .disabled(!session.connected || tab.selection.count != 1)
      }
      HStack(spacing: 4) {
        Image(systemName: state.symbol).foregroundStyle(state.color)
        Text(state.label)
        if session.runActive, let now = activity { Text("· " + now).foregroundStyle(.secondary).lineLimit(1) }
      }
      .accessibilityElement(children: .combine)
      if session.runStatus != nil {
        SwiftUI.TimelineView(.periodic(from: .now, by: 0.5)) { _ in
          Text(elapsed).monospacedDigit().foregroundStyle(.secondary)
            .help("Run の受付から（準備を含む）の経過時間。実行環境の時計で計ります。")
        }
      }
      Button { showIssues.toggle() } label: {
        HStack(spacing: 3) {
          Image(systemName: problems > 0 ? "exclamationmark.triangle.fill" : "checkmark.seal")
          Text("問題 \(problems) 件")
        }
        .foregroundStyle(problems > 0 ? .orange : .secondary)
      }
      .buttonStyle(.borderless)
      .help("問題一覧（対象の Card やログへ移動できます）")
      .popover(isPresented: $showIssues) {
        IssueList(workspace: workspace, session: session, tab: tab).frame(width: 460, height: 320)
      }
      if session.runActive {
        Image(systemName: "lock.fill").foregroundStyle(.secondary).help("実行中は Card を変更できません")
      }
    }
    .font(.callout)
    .padding(.horizontal, 10).padding(.vertical, 6)
    .background(.regularMaterial, in: Capsule())
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
struct CanvasDraftControls: View {
  @Environment(AppModel.self) private var model
  @Bindable var workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab

  var body: some View {
    let drafts = session.cards(in: tab.scope).compactMap { $0["id"]?.string }.map { key(tab.scope + [$0]) }
      .filter { session.kind($0) == .draft }
    if !drafts.isEmpty {
      HStack(spacing: 8) {
        Label("Draft \(drafts.count) 件", systemImage: "pencil.and.outline")
        Button {
          workspace.showChat = true
          model.chat(for: session).generateDrafts(scope: tab.scope, cards: drafts)
        } label: { Label("Card を生成", systemImage: "wand.and.stars") }
          .disabled(session.dirty || !session.connected || session.runActive || model.chat(for: session).busy)
          .help(session.dirty ? "先に変更を保存してください（配線と Draft の内容を確定してから生成します）" :
                  "この階層の Draft をチャットで実装します。進捗はチャットに表示されます。")
      }
      .font(.callout)
      .padding(.horizontal, 10).padding(.vertical, 6)
      .background(.regularMaterial, in: Capsule())
    }
  }
}
