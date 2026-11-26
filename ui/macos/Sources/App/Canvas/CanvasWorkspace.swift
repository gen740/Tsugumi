import AppKit
import SwiftUI
import TsugumiRPC
import TsugumiViewPresentation

/// The canvas with its floating controls. The canvas reaches the top of the
/// window; the project's title (with where the canvas is), the run control
/// and the project's actions float on it with the same look.
struct CanvasWorkspace: View {
  @Bindable var workspace: Workspace
  let session: ProjectSession
  @Bindable var tab: WorkflowTab
  let showsCardViews: Bool
  /// Where the window buttons and the sidebar button end (title bar).
  static let besideSidebarButton: CGFloat = 142
  @State private var detailLeading: CGFloat = 0

  var body: some View {
    ZStack(alignment: .top) {
      GraphCanvas(workspace: workspace, session: session, tab: tab)
        .ignoresSafeArea(.container, edges: .top)
      if showsCardViews, session.cardViews.hasPanels {
        CanvasCardViews(presentation: session.cardViews)
      }
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
        CanvasRunControl(session: session, tab: tab).fixedSize().layoutPriority(2)
        Spacer(minLength: 0)
        CanvasDraftControls(workspace: workspace, session: session, tab: tab).fixedSize().layoutPriority(1)
        CanvasCommentControls(workspace: workspace, session: session, tab: tab).fixedSize().layoutPriority(1)
        if !workspace.showChat {
          ChatToggle(open: false) { workspace.showChat = true }
            .padding(.horizontal, 12).padding(.vertical, 7)
            .background(.regularMaterial, in: Capsule())
            .fixedSize().layoutPriority(2)
        }
      }
      // In the title bar's row, beside the window buttons: right of the
      // sidebar button while the sidebar is closed.
      .padding(.leading, max(10, Self.besideSidebarButton - detailLeading))
      .padding(.trailing, 10)
      .padding(.top, 11)
      .ignoresSafeArea(.container, edges: .top)
    }
    .onGeometryChange(for: CGFloat.self) { $0.frame(in: .global).minX } action: { detailLeading = $0 }
    // At narrow widths the status moves above the left controls.
    .overlay(alignment: .bottom) {
      CanvasBottomControls {
        viewControls.fixedSize()
      } status: {
        StatusBar(workspace: workspace, session: session, tab: tab)
      }
      .padding(10)
    }
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
            .frame(minWidth: 48)
          // No room for even part of a name: a mark, not a sliver of a letter.
          Text(verbatim: "…").fontWeight(.semibold).fixedSize()
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
      Text("\(Int((zoom * 100).rounded()))%").monospacedDigit().frame(minWidth: 42)
      Button { Task { await session.arrange(tab.scope) } } label: { Image(systemName: "rectangle.grid.2x2") }
        .help("整列（配置だけを変更）")
      Divider().frame(height: 16)
      NixReferenceControls(tab: tab)
    }
    .buttonStyle(.borderless)
    .padding(.horizontal, 10).padding(.vertical, 5)
    .background(.regularMaterial, in: Capsule())
    .guideTarget("view", radius: 14)
  }

}

/// Live Card views float at a readable scale while the graph pans and zooms.
private struct CanvasCardViews: View {
  let presentation: CardViewPresentation
  @State private var front: UUID?

  var body: some View {
    GeometryReader { geo in
      let panels = presentation.panelOpenings
      let area = CGRect(x: 16, y: 48, width: max(0, geo.size.width - 32), height: max(0, geo.size.height - 92))
      let gap: CGFloat = 12
      let columns = panels.count > 1 && area.width >= 620 ? 2 : 1
      let rows = panels.count > columns && area.height >= 440 ? 2 : 1
      let width = min(680, max(0, (area.width - gap * CGFloat(columns - 1)) / CGFloat(columns)))
      let height = min(500, max(0, (area.height - gap * CGFloat(rows - 1)) / CGFloat(rows)))
      let stepX = width + gap, stepY = height + gap

      ForEach(Array(panels.enumerated()), id: \.element.id) { index, opening in
        let slot = index % (columns * rows)
        let stagger = CGFloat(min(4, index / (columns * rows))) * 18
        let center = CGPoint(x: area.midX + (CGFloat(slot % columns) - CGFloat(columns - 1) / 2) * stepX + stagger,
                             y: area.midY + (CGFloat(slot / columns) - CGFloat(rows - 1) / 2) * stepY + stagger)
        CanvasCardPanel(presentation: presentation, opening: opening, area: area,
                        size: CGSize(width: width, height: height), initialCenter: center) {
          front = opening.id
        }
        .zIndex(front == opening.id ? 1 : 0)
      }
    }
  }
}

private struct CanvasCardPanel: View {
  let presentation: CardViewPresentation
  let opening: CardViewOpening
  let area: CGRect
  let size: CGSize
  let initialCenter: CGPoint
  let bringForward: () -> Void
  @State private var movedCenter: CGPoint?
  @State private var drag = CGSize.zero

  var body: some View {
    let center = clamped(movedCenter ?? initialCenter)
    CardViewPanel(presentation: presentation, id: opening.id) {
      HStack(spacing: 6) {
        Image(systemName: "line.3.horizontal")
          .foregroundStyle(.secondary)
          .frame(width: 24, height: 24)
        Text(verbatim: "\(opening.card.joined(separator: "/")) / \(opening.name)")
          .font(.callout)
          .lineLimit(1)
        Spacer(minLength: 0)
      }
      .overlay {
        CardViewDragHandle(onPress: bringForward, onDrag: { drag = $0 }) { movement in
          drag = .zero
          guard movement != .zero else { return }
          movedCenter = clamped(CGPoint(x: center.x + movement.width, y: center.y + movement.height))
        }
        .accessibilityHidden(true)
      }
    }
    .frame(width: size.width, height: size.height)
    .background(.regularMaterial)
    .clipShape(RoundedRectangle(cornerRadius: 9))
    .overlay { RoundedRectangle(cornerRadius: 9).strokeBorder(.secondary.opacity(0.25)).allowsHitTesting(false) }
    .shadow(color: .black.opacity(0.22), radius: 12, y: 4)
    .position(clamped(CGPoint(x: center.x + drag.width, y: center.y + drag.height)))
  }

  private func clamped(_ point: CGPoint) -> CGPoint {
    CGPoint(x: min(area.maxX - size.width / 2, max(area.minX + size.width / 2, point.x)),
            y: min(area.maxY - size.height / 2, max(area.minY + size.height / 2, point.y)))
  }
}

/// The title captures the mouse while moving. Window coordinates stay fixed
/// even though the title itself changes position during the drag.
private struct CardViewDragHandle: NSViewRepresentable {
  let onPress: () -> Void
  let onDrag: (CGSize) -> Void
  let onRelease: (CGSize) -> Void

  func makeNSView(context: Context) -> Handle { Handle() }

  func updateNSView(_ view: Handle, context: Context) {
    view.onPress = onPress
    view.onDrag = onDrag
    view.onRelease = onRelease
  }

  final class Handle: NSView {
    var onPress: (() -> Void)?
    var onDrag: ((CGSize) -> Void)?
    var onRelease: ((CGSize) -> Void)?
    private var origin: CGPoint?

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func mouseDown(with event: NSEvent) {
      origin = event.locationInWindow
      onPress?()
    }

    override func mouseDragged(with event: NSEvent) {
      if let origin { onDrag?(translation(from: origin, to: event.locationInWindow)) }
    }

    override func mouseUp(with event: NSEvent) {
      guard let origin else { return }
      self.origin = nil
      onRelease?(translation(from: origin, to: event.locationInWindow))
    }

    private func translation(from start: CGPoint, to end: CGPoint) -> CGSize {
      CGSize(width: end.x - start.x, height: start.y - end.y)
    }
  }
}

struct ChatToggle: View {
  let open: Bool
  let action: () -> Void
  @Environment(GuideStage.self) private var stage: GuideStage?
  @State private var guideFrame = CGRect.zero

  var body: some View {
    Button(action: action) { Image(systemName: "bubble.left.and.text.bubble.right") }
      .buttonStyle(.borderless)
      .font(.callout)
      .help(open ? "チャットを閉じる（\(AppPreferences.shared.shortcut(.toggleChat).label)）"
                 : "チャットを開く（\(AppPreferences.shared.shortcut(.toggleChat).label)）")
      .accessibilityLabel("チャット")
      .guideTarget("chat")
      .onGeometryChange(for: CGRect.self) { $0.frame(in: .global) } action: { frame in
        guideFrame = frame
        stage?.corner = frame
      }
      .onDisappear { if stage?.corner == guideFrame { stage?.corner = .zero } }
  }
}

/// Run / stop, the run's state and its elapsed time. Per-Card counts and details are on the Cards, in the
/// history and on the timeline (a share of finished Cards says nothing about
/// a run with loops, so there is no progress bar).
struct CanvasRunControl: View {
  let session: ProjectSession
  let tab: WorkflowTab
  /// Shown as running: from the start, and for a moment after a quick run,
  /// so a short run reads as one step instead of a flicker.
  @State private var shownActive = false
  @State private var activeSince = Date.distantPast

  var body: some View {
    let state = session.runStatus?.state ?? .idle
    let label = session.stopRequested ? String(localized: "停止を要求中")
      : (shownActive && !session.runActive ? Tsugumi_Data_RunState.running.label : state.label)
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
      .disabled(shownActive ? !session.canStopRun : !session.connected)
      .help(shownActive ? tip(String(localized: "Workflow の実行を停止（チャットの回答停止とは別の操作です）"), .stop)
                        : tip(String(localized: "文書の Workflow 全体を開始 Card から実行（未保存の変更は先に保存します）"), .run))
      if let error = session.stopError {
        Label(error, systemImage: "exclamationmark.circle.fill")
          .foregroundStyle(.red)
          .lineLimit(2)
          .frame(width: 172, alignment: .leading)
          .help(error)
          .accessibilityElement(children: .combine)
      } else {
        // State and time on one line in a fixed width (what runs now is in the
        // tooltip); the clock ticks in hundredths while the run is active.
        HStack(spacing: 4) {
          Image(systemName: shownActive ? Tsugumi_Data_RunState.running.symbol : state.symbol)
            .foregroundStyle(shownActive ? Tsugumi_Data_RunState.running.color : state.color)
          Text(label).lineLimit(1)
          SwiftUI.TimelineView(.periodic(from: .now, by: session.runActive ? 0.03 : 1)) { _ in
            Text(elapsed).monospacedDigit().foregroundStyle(.secondary).fixedSize()
          }
        }
        .frame(width: 172, alignment: .leading)
        .help(shownActive ? (session.stopRequested ? label : (activity ?? state.label)) + String(localized: "（経過時間は Run の受付から、実行環境の時計で計ります）")
                          : String(localized: "Run の状態と経過時間"))
        .accessibilityElement(children: .combine)
      }

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
      let count = asked.reduce(0) { $0 + session.changeRequests($1).count }
      Button {
        workspace.showChat = true
        model.chat(for: session).generateFromComments(scope: tab.scope, cards: asked)
      } label: { Label("コメントから生成（\(count)）", systemImage: "bubble.left") }
        .guideTarget("from_comments")
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
        .guideTarget("generate")
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

/// Give the status the whole row when it no longer fits beside the view controls.
struct CanvasBottomControls<Controls: View, Status: View>: View {
  @ViewBuilder var controls: () -> Controls
  @ViewBuilder var status: () -> Status

  var body: some View {
    ViewThatFits(in: .horizontal) {
      HStack(alignment: .bottom, spacing: 8) {
        controls().fixedSize()
        Spacer(minLength: 0)
        status()
      }
      VStack(alignment: .leading, spacing: 8) {
        status().frame(maxWidth: .infinity, alignment: .trailing)
        controls().fixedSize()
      }
    }
  }
}
