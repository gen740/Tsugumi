import AppKit
import SwiftUI
import TsugumiRPC
import TsugumiViewPresentation

/// The canvas with its actions, notices and status bar.
struct CanvasWorkspace: View {
  @Bindable var workspace: Workspace
  let session: ProjectSession
  @Bindable var tab: WorkflowTab
  let showsCardViews: Bool
  @State private var bottomControlsHeight: CGFloat = 0

  var body: some View {
    GraphCanvas(workspace: workspace, session: session, tab: tab, showsCardViews: showsCardViews,
                bottomControlsHeight: bottomControlsHeight)
      .overlay(alignment: .topTrailing) {
        VStack(alignment: .trailing, spacing: 8) {
          if let notice = session.notice {
            noticeBanner(notice) { session.notice = nil }
          }
          if let error = session.stopError {
            noticeBanner(.error(error))
          }
          HStack(spacing: 8) {
            CanvasDraftControls(workspace: workspace, session: session, tab: tab).fixedSize()
            CanvasCommentControls(workspace: workspace, session: session, tab: tab).fixedSize()
          }
        }
        .padding(10)
      }
    // At narrow widths the status moves above the left controls.
    .overlay(alignment: .bottom) {
      CanvasBottomControls {
        viewControls.fixedSize()
      } status: {
        StatusBar(workspace: workspace, session: session, tab: tab)
      }
      .padding(10)
      .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { bottomControlsHeight = $0 }
    }
  }

  private func noticeBanner(_ notice: WorkbenchNotice, dismiss: (() -> Void)? = nil) -> some View {
    HStack(alignment: .top, spacing: 8) {
      Label(notice.message, systemImage: notice.symbol)
        .foregroundStyle(notice.color)
        .lineLimit(3)
        .help(notice.message)
        .textSelection(.enabled)
      if let dismiss {
        Button(action: dismiss) { Image(systemName: "xmark") }
          .buttonStyle(.borderless)
          .accessibilityLabel("閉じる")
      }
    }
    .font(.caption)
    .padding(8)
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 9))
    .frame(maxWidth: 420, alignment: .trailing)
  }

  private var viewControls: some View {
    let zoom = session.cameras[key(tab.scope)]?.zoom ?? 1
    return HStack(spacing: 2) {
      Text("\(Int((zoom * 100).rounded()))%").monospacedDigit().frame(minWidth: 42)
      Button { Task { await session.arrange(tab.scope) } } label: { Image(systemName: "rectangle.grid.2x2") }
        .help("整列（配置だけを変更）")
    }
    .buttonStyle(.borderless)
    .padding(.horizontal, 10).padding(.vertical, 5)
    .background(.regularMaterial, in: Capsule())
    .guideTarget("view", radius: 14)
  }
}

/// The project's title and where the canvas is (Card in Card), in one place.
struct CanvasBreadcrumbs: View {
  let session: ProjectSession
  @Bindable var tab: WorkflowTab

  var body: some View {
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
    .fixedSize(horizontal: true, vertical: false)
    .font(.callout)
    .workbenchToolbarCapsule()
  }

  private func goUp() {
    if !tab.back.isEmpty { tab.goBack() } else if !tab.scope.isEmpty { tab.enter(Array(tab.scope.dropLast())) }
  }
}

extension View {
  func workbenchToolbarCapsule() -> some View {
    padding(.horizontal, 10).frame(height: 34)
      .background(.regularMaterial, in: Capsule())
  }
}

/// A Card view's contents are laid out in world units. The enclosing canvas
/// supplies its position and scale, including during a live camera gesture.
/// One surface: a thin title line (dragging it moves the panel, a double
/// click shows the view in a window) over the view. It has no close button:
/// the Card's menu folds it away.
struct CanvasCardPanel: View {
  let presentation: CardViewPresentation
  let opening: CardViewOpening
  let onPress: () -> Void
  let onDrag: (CGSize) -> Void
  let onRelease: (CGSize) -> Void

  var body: some View {
    VStack(spacing: 0) {
      Text(verbatim: "\(opening.card.joined(separator: "/")) / \(opening.name)")
        .font(.caption)
        .foregroundStyle(.secondary)
        .lineLimit(1)
        .truncationMode(.middle)
        .padding(.horizontal, 8)
        .frame(maxWidth: .infinity, minHeight: 20, maxHeight: 20, alignment: .leading)
        .overlay {
          CardViewDragHandle(onPress: onPress, onDrag: onDrag, onRelease: onRelease,
                             onDoubleClick: { presentation.detachPanel(opening.id) })
            .accessibilityHidden(true)
        }
        .help("ドラッグで移動、ダブルクリックで別ウインドウに表示")
      CardViewPanelContent(presentation: presentation, id: opening.id)
    }
    .background(.regularMaterial)
    .clipShape(RoundedRectangle(cornerRadius: 9))
    .overlay { RoundedRectangle(cornerRadius: 9).strokeBorder(.secondary.opacity(0.25)).allowsHitTesting(false) }
    .shadow(color: .black.opacity(0.22), radius: 12, y: 4)
  }
}

/// The title captures the mouse while moving. Window coordinates stay fixed
/// even though the title itself changes position during the drag.
private struct CardViewDragHandle: NSViewRepresentable {
  let onPress: () -> Void
  let onDrag: (CGSize) -> Void
  let onRelease: (CGSize) -> Void
  let onDoubleClick: () -> Void

  func makeNSView(context: Context) -> Handle { Handle() }

  func updateNSView(_ view: Handle, context: Context) {
    view.onPress = onPress
    view.onDrag = onDrag
    view.onRelease = onRelease
    view.onDoubleClick = onDoubleClick
  }

  final class Handle: NSView {
    var onPress: (() -> Void)?
    var onDrag: ((CGSize) -> Void)?
    var onRelease: ((CGSize) -> Void)?
    var onDoubleClick: (() -> Void)?
    private var origin: CGPoint?
    private var dragInProgress: ((CGSize) -> Void)?
    private var finishDrag: ((CGSize) -> Void)?

    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func mouseDown(with event: NSEvent) {
      if event.clickCount == 2 {
        origin = nil
        onDoubleClick?()
        return
      }
      origin = event.locationInWindow
      dragInProgress = onDrag
      finishDrag = onRelease
      onPress?()
    }

    override func mouseDragged(with event: NSEvent) {
      if let origin { dragInProgress?(translation(from: origin, to: event.locationInWindow)) }
    }

    override func mouseUp(with event: NSEvent) {
      guard let origin else { return }
      self.origin = nil
      finishDrag?(translation(from: origin, to: event.locationInWindow))
      dragInProgress = nil
      finishDrag = nil
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
      .help(open ? "チャットを閉じる（\(AppPreferences.shared.shortcut(.toggleChat).label)）"
                 : "チャットを開く（\(AppPreferences.shared.shortcut(.toggleChat).label)）")
      .accessibilityLabel(open ? "チャットを閉じる" : "チャットを開く")
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
      Button {
        if session.runActive { Task { await session.cancelRun() } } else { RunActions.runAll(session, scope: tab.scope) }
      } label: {
        Image(systemName: shownActive ? "stop.fill" : "play.fill")
          .frame(width: 24, height: 24)
          .background(Circle().strokeBorder(lineWidth: 1))
      }
      .buttonStyle(.borderless)
      .foregroundStyle(shownActive ? Color.red : Color.accentColor)
      .guideTarget("run")
      .disabled(shownActive ? !session.canStopRun : !session.connected)
      .accessibilityLabel(shownActive ? "停止" : "実行")
      .help(shownActive ? tip(String(localized: "Workflow の実行を停止（チャットの回答停止とは別の操作です）"), .stop)
                        : tip(String(localized: "文書の Workflow 全体を開始 Card から実行（未保存の変更は先に保存します）"), .run))
      Divider().frame(height: 16)
      HStack(spacing: 6) {
        Group {
          if shownActive {
            ProgressView().controlSize(.mini)
          } else {
            Image(systemName: state.symbol).foregroundStyle(state.color)
          }
        }
        .frame(width: 15, height: 15)
        .accessibilityHidden(true)
        SwiftUI.TimelineView(.periodic(from: .now, by: session.runActive ? 0.03 : 1)) { _ in
          Text(elapsed).monospacedDigit().foregroundStyle(.secondary).fixedSize()
        }
      }
      .help(label + (shownActive && !session.stopRequested ? activity.map { " · " + $0 } ?? "" : ""))
      .accessibilityElement(children: .combine)
      .accessibilityLabel(label)
      .accessibilityValue(elapsed)
    }
    .font(.callout)
    .workbenchToolbarCapsule()
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
    guard let s = session.runStatus, let start = s.acceptedAt ?? s.startedAt else { return formatDuration(0) }
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
