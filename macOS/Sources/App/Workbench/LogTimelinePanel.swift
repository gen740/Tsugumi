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
        Picker("", selection: $workspace.bottomPanel) {
          ForEach(BottomPanel.allCases) { Text($0.title).tag($0) }
        }
        .pickerStyle(.segmented).labelsHidden().frame(width: 200)
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
        Button { workspace.showBottom = false } label: { Image(systemName: "xmark") }
          .buttonStyle(.borderless).help("閉じる")
      }
      .padding(6)
      Divider()
      switch workspace.bottomPanel {
      case .log:
        if let runId {
          RunLogView(session: session, runId: runId, card: workspace.logCard.map(path), invocation: nil)
        } else {
          ContentUnavailableView("実行履歴はありません", systemImage: "text.alignleft", description: Text("実行するとここにログが表示されます。"))
        }
      case .timeline:
        RunTimelineView(session: session, runId: runId, onSelect: { k in
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
  @State private var stream: Tsugumi_Data_LogStream = .stdout
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
          Text(log.isEmpty ? "ログはありません" : log)
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

/// Invocations of one run on a shared time axis, one row per Card.
struct RunTimelineView: View {
  let session: ProjectSession
  let runId: String?
  let onSelect: (CardKey) -> Void
  @State private var entries: [TimelineEntry] = []
  @State private var loading = false

  var body: some View {
    let list = runId == session.currentRunId && !session.timeline.isEmpty ? session.timeline : entries
    let rows = Array(Set(list.map(\.card))).sorted()
    let start = list.compactMap(\.startedAt).min() ?? 0
    let now = Int64(Date().timeIntervalSince1970 * 1000)
    let end = max(start + 1, list.map { $0.endedAt ?? (session.runActive ? now : ($0.startedAt ?? start)) }.max() ?? start + 1)
    Group {
      if list.isEmpty {
        if loading { ProgressView() } else {
          ContentUnavailableView("実行状況はありません", systemImage: "chart.bar.xaxis", description: Text("実行を選ぶか、実行してください。"))
        }
      } else {
        ScrollView {
          VStack(alignment: .leading, spacing: 3) {
            HStack {
              Text("").frame(width: 180)
              Text("0 秒").font(.caption2).foregroundStyle(.secondary)
              Spacer()
              Text(formatDuration(Double(end - start) / 1000)).font(.caption2).foregroundStyle(.secondary)
            }
            ForEach(rows, id: \.self) { k in
              HStack(spacing: 6) {
                Text(session.displayName(k)).font(.caption).lineLimit(1).frame(width: 180, alignment: .leading).help(k)
                GeometryReader { g in
                  ZStack(alignment: .leading) {
                    Rectangle().fill(.quaternary.opacity(0.4))
                    ForEach(list.filter { $0.card == k }) { e in
                      let s = e.startedAt ?? start
                      let f = e.endedAt ?? (session.runActive ? now : s)
                      let x = CGFloat(s - start) / CGFloat(end - start) * g.size.width
                      let w = max(3, CGFloat(f - s) / CGFloat(end - start) * g.size.width)
                      RoundedRectangle(cornerRadius: 2)
                        .fill(e.state.color.opacity(0.8))
                        .frame(width: w, height: g.size.height - 4)
                        .offset(x: x)
                        .help("#\(e.invocation) · \(e.state.label) · \(formatDuration(Double(f - s) / 1000))")
                        .onTapGesture { onSelect(k) }
                    }
                  }
                }
                .frame(height: 16)
              }
            }
          }
          .padding(8)
        }
      }
    }
    .task(id: runId) { await load() }
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
        out.append(TimelineEntry(card: k, invocation: r.invocation ?? 0, state: r.state, startedAt: r.startedAt, endedAt: r.endedAt))
      }
    }
    entries = out
  }
}
