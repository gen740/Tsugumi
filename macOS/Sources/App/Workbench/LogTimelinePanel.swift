import AppKit
import SwiftUI
import TsugumiRPC

/// Bottom panel: the whole run log, or the run's Cards on a time axis.
struct LogTimelinePanel: View {
  @Bindable var workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab
  @State private var runs: [Tsugumi_RPC_RunSummaryT] = []
  @State private var runId: String?

  var body: some View {
    VStack(spacing: 0) {
      HStack(spacing: 10) {
        Label(workspace.bottomPanel.title, systemImage: workspace.bottomPanel == .log ? "text.alignleft" : "chart.bar.xaxis")
          .font(.callout.weight(.medium))
        Picker("実行", selection: $runId) {
          if runs.isEmpty { Text("実行履歴なし").tag(String?.none) }
          ForEach(runs, id: \.status.runId) { r in
            Text("\(formatMillis(r.status.acceptedAt)) · \(r.status.state.label)").tag(Optional(r.status.runId))
          }
        }
        .frame(maxWidth: 320)
        if workspace.bottomPanel == .log {
          Picker("Card", selection: $workspace.logCard) {
            Text("全体").tag(CardKey?.none)
            ForEach(session.resolution.keys.sorted(), id: \.self) { k in Text(k).tag(Optional(k)) }
          }
          .frame(maxWidth: 240)
        }
        Spacer()
        Button { workspace.showBottom = false } label: { Image(systemName: "xmark").font(.caption) }
          .buttonStyle(.borderless).help("閉じる（下部のアイコンでも開閉できます）")
      }
      .padding(6)
      Divider()
      switch workspace.bottomPanel {
      case .log:
        if let runId, let card = workspace.logCard {
          StageLogView(session: session, runId: runId, card: path(card), invocation: workspace.logInvocation, focus: workspace.logStage)
        } else if let runId {
          // The whole run: its own journal (Card logs are shown by stage).
          RunLogView(session: session, runId: runId, card: nil, invocation: nil, stream: $workspace.logStream)
        } else {
          ContentUnavailableView("実行履歴はありません", systemImage: "text.alignleft", description: Text("実行するとここにログが表示されます。"))
        }
      case .timeline:
        RunTimelineView(session: session, runId: runId, run: runs.first { $0.status.runId == runId }?.status, onSelect: { k in
          let scope = Array(path(k).dropLast())
          tab.enter(scope)
          tab.selection = [k]
          workspace.showLog(for: k)
        })
      }
    }
    .task(id: session.lastFinished) { await loadRuns() }
    .onChange(of: session.runStatus?.runId) { _, id in
      if let id {
        runId = id
        Task { await loadRuns() }
      }
    }
  }

  private func loadRuns() async {
    guard let rpc = session.rpc else { return }
    runs = (try? await rpc.history(limit: 50).runs.compactMap { $0 }) ?? []
    if runId == nil || !runs.contains(where: { $0.status.runId == runId }) { runId = session.currentRunId ?? runs.first?.status.runId }
  }
}

/// Log text of a run (or one Card / invocation of it), read incrementally
/// with LogRange from its own next_offset when LogAvailable arrives.
struct RunLogView: View {
  let session: ProjectSession
  let runId: String
  let card: [String]?
  let invocation: UInt64?
  @Binding var stream: Tsugumi_Data_LogStream
  @State private var log = ""
  @State private var offset: UInt64 = 0
  @State private var scopeKey = ""
  @State private var error: String?
  @State private var follow = true

  private var identity: String { "\(runId)|\(stream.rawValue)|\(card?.joined(separator: "/") ?? "")|\(invocation ?? 0)" }

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      HStack {
        Picker("", selection: $stream) {
          Text("stdout").tag(Tsugumi_Data_LogStream.stdout)
          Text("stderr").tag(Tsugumi_Data_LogStream.stderr)
          Text("journal").tag(Tsugumi_Data_LogStream.journal)
        }
        .pickerStyle(.segmented).frame(width: 240).labelsHidden()
        Text(card.map { "Card \($0.joined(separator: "/"))" + (invocation.map { " #\($0)" } ?? "") } ?? "実行全体")
          .font(.caption).foregroundStyle(.secondary)
        Spacer()
        Toggle("末尾を追う", isOn: $follow).toggleStyle(.checkbox).font(.caption)
        Button("コピー") {
          NSPasteboard.general.clearContents()
          NSPasteboard.general.setString(log, forType: .string)
        }
        .disabled(log.isEmpty)
        Button("書き出す…") { export() }.disabled(log.isEmpty)
      }
      .padding(.horizontal, 6)
      ScrollViewReader { proxy in
        ScrollView {
          Text(log.isEmpty ? emptyReason : log)
            .font(.system(size: AppPreferences.shared.editorFontSize - 1, design: .monospaced))
            .foregroundStyle(log.isEmpty ? .secondary : .primary)
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(6)
          Color.clear.frame(height: 1).id("end")
        }
        .onChange(of: log) { if follow { proxy.scrollTo("end") } }
      }
      if let error { Text(error).font(.caption).foregroundStyle(.red).padding(.horizontal, 6) }
    }
    .task(id: identity) { await reset() }
    .onChange(of: session.logRevision) { Task { await readMore() } }
  }

  /// Why nothing is shown, so an empty pane is never ambiguous.
  private var emptyReason: String {
    if let error { return String(localized: "ログを読み込めませんでした: ") + error }
    if let card, runId == session.currentRunId, session.cardStates[key(card)] == nil {
      return String(localized: "この実行では、この Card はまだ動いていません。")
    }
    switch stream {
    case .stderr: return String(localized: "stderr への出力はありません（Nix 評価・build のログもここに出ます）。")
    case .journal: return String(localized: "記録された段階の出来事はありません。")
    default: return String(localized: "stdout への出力はありません。")
    }
  }

  private func reset() async {
    log = ""
    offset = 0
    scopeKey = identity
    await readMore()
  }

  private func readMore() async {
    guard let rpc = session.rpc else { return }
    let scope = scopeKey
    do {
      var inv = invocation
      if let card, inv == nil {
        inv = session.cardStates[key(card)]?.invocation
        if inv == nil { inv = try? await rpc.record(runId: runId, card: card).selectedInvocation }
      }
      while true {
        let slice = try await rpc.logRange(runId: runId, card: card, invocation: card == nil ? nil : inv,
                                           stream: stream, offset: offset, limit: 256 * 1024)
        guard scope == scopeKey else { return }  // the selection changed meanwhile
        log += slice.text
        let next = slice.nextOffset ?? offset
        if next == offset { break }
        offset = next
        if next >= (slice.sizeBytes ?? 0) { break }
      }
      error = nil
    } catch let f as RPCFailure where f.kind == .notFound {
      error = nil
    } catch {
      self.error = "\(error)"
    }
  }

  private func export() {
    let p = NSSavePanel()
    p.nameFieldStringValue = "\(runId)-\(stream).log"
    if p.runModal() == .OK, let url = p.url { try? Data(log.utf8).write(to: url) }
  }
}

/// Invocations of one run on a shared time axis, one row per Card, each bar
/// split into its stages. Drag over a time range to zoom into it, scroll
/// sideways to move along time, 「全体」 to see the whole run again. Hover a bar
/// for its Card, invocation, stage and times; click it to show the Card.
struct RunTimelineView: View {
  let session: ProjectSession
  let runId: String?
  /// Status of the chosen run (past runs); the current run uses the session's.
  var run: Tsugumi_RPC_RunStatusT?
  let onSelect: (CardKey) -> Void
  @State private var entries: [TimelineEntry] = []
  @State private var loading = false
  /// Shown time range in ms from the start of the run (nil: the whole run).
  @State private var window: ClosedRange<Double>?
  @State private var dragFrom: CGFloat?
  @State private var dragTo: CGFloat?
  @State private var hoverPoint: CGPoint?

  private static let labelWidth: CGFloat = 170
  private static let rowHeight: CGFloat = 18

  var body: some View {
    let list = runId == session.currentRunId && !session.timeline.isEmpty ? session.timeline : entries
    let rows = Array(Set(list.map(\.card))).sorted()
    // The axis starts when the run was accepted and ends when it ended (or
    // now, on the core's clock), like the elapsed time of the run control.
    let status = runId == session.currentRunId ? session.runStatus : run
    let live = runId == session.currentRunId && session.runActive
    let now = session.coreNow()
    let start = status?.acceptedAt ?? list.compactMap(\.startedAt).min() ?? 0
    let lastEnd = list.map { $0.endedAt ?? (live ? now : ($0.startedAt ?? start)) }.max() ?? start
    let end = max(start + 1, status?.endedAt ?? (live ? now : lastEnd), lastEnd)
    let total = Double(end - start)
    let shown = window ?? 0...total
    Group {
      if list.isEmpty {
        if loading { ProgressView() } else {
          ContentUnavailableView("実行状況はありません", systemImage: "chart.bar.xaxis", description: Text("実行を選ぶか、実行してください。"))
        }
      } else {
        VStack(spacing: 0) {
          HStack(spacing: 8) {
            Text(formatDuration(shown.lowerBound / 1000)).font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
              .frame(width: Self.labelWidth, alignment: .trailing)
            Spacer()
            Text("ドラッグで範囲を拡大・横スクロールで移動").font(.caption2).foregroundStyle(.tertiary)
            Button("全体") { window = nil }.controlSize(.small).disabled(window == nil)
              .help("Run 全体（受付から終了まで \(formatDuration(total / 1000))）を表示")
            Text(formatDuration(shown.upperBound / 1000) + (live && window == nil ? String(localized: "（実行中）") : ""))
              .font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
          }
          .padding(.horizontal, 8).padding(.vertical, 4)
          ScrollView(.vertical) {
            HStack(alignment: .top, spacing: 0) {
              VStack(alignment: .leading, spacing: 0) {
                ForEach(rows, id: \.self) { k in
                  let n = list.filter { $0.card == k }.count
                  HStack(spacing: 4) {
                    Text(session.displayName(k)).lineLimit(1)
                    Spacer(minLength: 2)
                    if n > 1 { Text("×\(n)").foregroundStyle(.secondary).monospacedDigit() }
                  }
                  .font(.caption)
                  .frame(width: Self.labelWidth - 12, height: Self.rowHeight, alignment: .leading)
                  .padding(.leading, 8)
                  .help(k)
                  .onTapGesture { onSelect(k) }
                }
              }
              GeometryReader { g in
                chart(list: list, rows: rows, start: start, live: live, now: now, shown: shown, total: total, width: g.size.width)
              }
              .frame(height: CGFloat(rows.count) * Self.rowHeight)
            }
          }
        }
      }
    }
    .task(id: runId) { window = nil; await load() }
  }

  private func chart(list: [TimelineEntry], rows: [CardKey], start: Int64, live: Bool, now: Int64,
                     shown: ClosedRange<Double>, total: Double, width: CGFloat) -> some View {
    let span = max(1, shown.upperBound - shown.lowerBound)
    func x(_ t: Int64) -> CGFloat { CGFloat((Double(t - start) - shown.lowerBound) / span) * width }
    let rowIndex = Dictionary(uniqueKeysWithValues: rows.enumerated().map { ($1, $0) })
    let hovered = hoverPoint.flatMap { p in hit(list, rowIndex, p, x: x) }
    return ZStack(alignment: .topLeading) {
      Canvas { ctx, size in
        for (i, _) in rows.enumerated() where i % 2 == 1 {
          ctx.fill(Path(CGRect(x: 0, y: CGFloat(i) * Self.rowHeight, width: size.width, height: Self.rowHeight)),
                   with: .color(.secondary.opacity(0.06)))
        }
        for e in list {
          guard let row = rowIndex[e.card], let s = e.startedAt else { continue }
          let f = e.endedAt ?? (live ? now : s)
          let x0 = x(s), x1 = max(x0 + 2, x(f))
          guard x1 >= 0, x0 <= size.width else { continue }
          let y = CGFloat(row) * Self.rowHeight + 3, h = Self.rowHeight - 6
          ctx.fill(Path(roundedRect: CGRect(x: x0, y: y, width: x1 - x0, height: h), cornerRadius: 2), with: .color(e.state.color.opacity(0.35)))
          // Stages inside the invocation, each in its own colour.
          for st in e.stages {
            guard let a = st.start else { continue }
            let b = st.end ?? (live ? now : a)
            let sx0 = x(a), sx1 = max(sx0 + 1, x(b))
            ctx.fill(Path(CGRect(x: sx0, y: y, width: sx1 - sx0, height: h)), with: .color(Self.color(st.stage).opacity(0.85)))
          }
        }
        if let a = dragFrom, let b = dragTo {
          ctx.fill(Path(CGRect(x: min(a, b), y: 0, width: abs(b - a), height: size.height)), with: .color(Color.accentColor.opacity(0.18)))
        }
      }
      if let h = hovered, let p = hoverPoint {
        Text(description(h)).font(.caption).padding(6)
          .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 6))
          .fixedSize()
          .offset(x: min(p.x + 12, max(0, width - 260)), y: p.y + 12)
          .allowsHitTesting(false)
      }
    }
    .contentShape(Rectangle())
    .onContinuousHover { phase in
      if case .active(let p) = phase { hoverPoint = p } else { hoverPoint = nil }
    }
    .gesture(DragGesture(minimumDistance: 4)
      .onChanged { v in dragFrom = v.startLocation.x; dragTo = v.location.x; hoverPoint = nil }
      .onEnded { v in
        let a = min(v.startLocation.x, v.location.x), b = max(v.startLocation.x, v.location.x)
        if b - a > 6 {
          let lo = shown.lowerBound + Double(a / width) * span, hi = shown.lowerBound + Double(b / width) * span
          window = max(0, lo)...max(lo + 1, hi)
        }
        dragFrom = nil
        dragTo = nil
      })
    .onTapGesture { if let h = hovered { onSelect(h.card) } }
    .overlay(HorizontalScrollCatcher { dx in
      // Sideways scroll moves along time (only when zoomed in).
      guard let w = window else { return }
      let shift = -Double(dx / max(width, 1)) * span
      let lo = min(max(0, w.lowerBound + shift), max(0, total - span))
      window = lo...(lo + span)
    })
  }

  private func hit(_ list: [TimelineEntry], _ rows: [CardKey: Int], _ p: CGPoint, x: (Int64) -> CGFloat) -> TimelineEntry? {
    let row = Int(p.y / Self.rowHeight)
    return list.last { e in
      guard rows[e.card] == row, let s = e.startedAt else { return false }
      let x0 = x(s), x1 = max(x0 + 2, x(e.endedAt ?? s))
      return p.x >= x0 - 2 && p.x <= x1 + 2
    }
  }

  private func description(_ e: TimelineEntry) -> String {
    var lines = ["\(session.displayName(e.card)) · 実行 #\(e.invocation) · \(e.state.label)"]
    if let s = e.startedAt, let f = e.endedAt { lines.append(String(localized: "全体 ") + formatDuration(Double(f - s) / 1000)) }
    for st in e.stages {
      if let a = st.start, let b = st.end { lines.append("\(st.stage.label): " + formatDuration(Double(b - a) / 1000)) }
    }
    return lines.joined(separator: "\n")
  }

  static func color(_ s: Stage) -> Color {
    switch s {
    case .prepare: return .indigo
    case .evaluate: return .cyan
    case .build: return .blue
    case .execute: return .green
    case .validate: return .mint
    case .save: return .teal
    default: return .gray
    }
  }

  /// A past run's invocations, read per Card with RunInvocations.
  private func load() async {
    entries = []
    guard let runId, runId != session.currentRunId || session.timeline.isEmpty, let rpc = session.rpc else { return }
    loading = true
    defer { loading = false }
    var out: [TimelineEntry] = []
    for k in session.resolution.keys.sorted().prefix(300) {
      guard let page = try? await rpc.invocations(runId: runId, card: path(k)) else { continue }
      for r in page.records.compactMap({ $0 }) {
        out.append(TimelineEntry(card: k, invocation: r.invocation ?? 0, state: r.state, startedAt: r.startedAt, endedAt: r.endedAt,
                                 stages: TimelineEntry.stages(r.stages)))
      }
    }
    entries = out
  }
}

/// Sideways two-finger (or shift + wheel) scroll, for the timeline.
struct HorizontalScrollCatcher: NSViewRepresentable {
  let onScroll: (CGFloat) -> Void
  func makeNSView(context: Context) -> NSView { V(onScroll) }
  func updateNSView(_ v: NSView, context: Context) { (v as? V)?.onScroll = onScroll }

  final class V: NSView {
    var onScroll: (CGFloat) -> Void
    init(_ f: @escaping (CGFloat) -> Void) { onScroll = f; super.init(frame: .zero) }
    required init?(coder: NSCoder) { fatalError() }
    override func hitTest(_ point: NSPoint) -> NSView? {
      // Only sideways scrolls; vertical ones scroll the rows, clicks pass through.
      guard let e = NSApp.currentEvent, e.type == .scrollWheel else { return nil }
      return abs(e.scrollingDeltaX) > abs(e.scrollingDeltaY) || e.modifierFlags.contains(.shift) ? self : nil
    }
    override func scrollWheel(with e: NSEvent) {
      onScroll(e.modifierFlags.contains(.shift) && e.scrollingDeltaX == 0 ? e.scrollingDeltaY : e.scrollingDeltaX)
    }
  }
}
