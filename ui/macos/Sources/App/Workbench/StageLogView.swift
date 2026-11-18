import AppKit
import SwiftUI
import TsugumiRPC

/// The log of one invocation of a Card by stage, like a CI job: SDK
/// preparation, Nix evaluation and build / execution / validation and save,
/// each with its state, duration and what it printed, folded unless it failed
/// or was asked for. The raw streams stay available under "By stream".
struct StageLogView: View {
  let session: ProjectSession
  let runId: String
  let card: [String]
  let invocation: UInt64?
  /// A stage to open (a click on a stage icon).
  var focus: Stage?
  @State private var stages: [StageRecord] = []
  @State private var state: Tsugumi_Data_CardState?
  @State private var failure: String?
  @State private var shownInvocation: UInt64?
  @State private var logs: [Tsugumi_Data_LogStream: Tsugumi_RPC_LogSliceT] = [:]
  @State private var truncatedAt: UInt64?
  @State private var exporting = false
  @State private var exportError: String?
  @State private var open: Set<String> = []
  /// Groups opened when they started running: each once, so one the reader folds stays folded.
  @State private var openedRunning: Set<String> = []
  @State private var rawStream: Tsugumi_Data_LogStream = .standardOutput
  @State private var error: String?
  @State private var loaded = false
  /// The log opens at its end and follows it, as the chat does, until the
  /// reader scrolls up; back at the end, it follows again.
  @State private var follow = ChatFollow()
  @State private var scrollPosition = ScrollPosition(edge: .bottom)

  private struct Refresh: Equatable {
    let runId: String
    let invocation: UInt64?
    let card: [String]
    let execution: ExecutionHistoryRefresh
  }

  private struct Group: Identifiable {
    let id: String
    let title: String
    let stages: [Stage]
  }

  private var groups: [Group] {
    let code = session.kind(key(card)) == .code
    return (code ? [Group(id: "build", title: String(localized: "SDK 準備・Nix 評価・build"), stages: [.prepare, .evaluate, .build])] : []) + [
      Group(id: "execute", title: String(localized: "実行"), stages: [.execute]),
      Group(id: "save", title: String(localized: "出力検証・保存"), stages: [.validate, .save]),
    ]
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      HStack {
        Text(String(localized: "\(session.displayName(key(card))) · 実行 #\(shownInvocation ?? invocation ?? 0)") + (state.map { " · " + $0.label } ?? ""))
          .font(.callout.weight(.medium))
        Spacer()
        Button("すべて開く") { open = Set(groups.map(\.id) + ["raw"]) }.controlSize(.small)
        Button("すべて閉じる") { open = [] }.controlSize(.small)
      }
      .padding(.horizontal, 8).padding(.vertical, 5)
      Divider()
      if !loaded {
        ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
      } else if let error {
        ContentUnavailableView {
          Label("ログを読み込めませんでした", systemImage: "exclamationmark.triangle")
        } description: { Text(error) } actions: { Button("再試行") { Task { await load() } } }
      } else if truncatedAt == nil && stages.isEmpty && logs.values.allSatisfy({ $0.text.isEmpty }) {
        ContentUnavailableView("この実行では、この Card はまだ動いていません", systemImage: "circle.dotted")
      } else {
        ScrollView {
          VStack(alignment: .leading, spacing: 4) {
            ForEach(groups) { g in section(g) }
            DisclosureGroup(isExpanded: binding("raw")) {
              VStack(alignment: .leading, spacing: 4) {
                Picker("", selection: $rawStream) {
                  Text("stdout").tag(Tsugumi_Data_LogStream.standardOutput)
                  Text("stderr").tag(Tsugumi_Data_LogStream.standardError)
                  Text("build").tag(Tsugumi_Data_LogStream.build)
                  Text("journal").tag(Tsugumi_Data_LogStream.journal)
                }
                .pickerStyle(.segmented).labelsHidden().frame(width: 300)
                logText(rawStream, empty: String(localized: "この stream への出力はありません"))
              }
            } label: {
              Label("stream 別（元の出力）", systemImage: "list.bullet.rectangle").font(.callout)
            }
            .padding(.horizontal, 8)
          }
          .padding(.vertical, 6)
        }
        .defaultScrollAnchor(.bottom, for: .initialOffset)
        .scrollPosition($scrollPosition)
        .onScrollGeometryChange(for: ChatFollow.View.self) { g in
          ChatFollow.View(top: g.visibleRect.minY, bottom: g.visibleRect.maxY, content: g.contentSize.height)
        } action: { _, now in
          if follow.update(now) { scrollPosition.scrollTo(edge: .bottom) }
        }
        .onScrollPhaseChange { _, phase in
          follow.readerScrolling = phase == .interacting || phase == .decelerating
          if phase == .idle, follow.following { scrollPosition.scrollTo(edge: .bottom) }
        }
      }
    }
    .onChange(of: "\(runId)|\(key(card))|\(invocation ?? 0)") {
      openedRunning = []
      stages = []
      state = nil
      failure = nil
      logs = [:]
      truncatedAt = nil
      exportError = nil
      loaded = false
      follow.restart()
      scrollPosition.scrollTo(edge: .bottom)
    }
    .task(id: Refresh(runId: runId, invocation: invocation, card: card,
                      execution: ExecutionHistoryRefresh(session: session, card: key(card)))) {
      // Read serially while this run is active. Log notifications can arrive
      // faster than a read finishes; restarting on each one starves the view.
      repeat {
        await load()
        guard !Task.isCancelled, session.runActive, runId == session.currentRunId else { return }
        do { try await Task.sleep(for: .milliseconds(500)) } catch { return }
      } while !Task.isCancelled
    }
    .onChange(of: focus) { if let f = focus, let g = groups.first(where: { $0.stages.contains(f) }) { open.insert(g.id) } }
  }

  private func binding(_ id: String) -> Binding<Bool> {
    Binding(get: { open.contains(id) }, set: { if $0 { open.insert(id) } else { open.remove(id) } })
  }

  private func records(_ g: Group) -> [StageRecord] { stages.filter { g.stages.contains($0.stage) } }

  private func status(_ g: Group) -> (String, Color, String) {
    let rs = records(g)
    if rs.contains(where: { $0.outcome == .failed }) { return ("xmark.circle.fill", .red, String(localized: "失敗")) }
    if rs.contains(where: { $0.outcome == .cancelled }) { return ("stop.circle.fill", .brown, String(localized: "停止")) }
    if rs.contains(where: { $0.outcome == .running }) { return ("circle.dotted.circle", .orange, String(localized: "進行中")) }
    if rs.isEmpty { return ("circle.dashed", .secondary, String(localized: "実行されていません")) }
    if rs.allSatisfy({ $0.outcome == .cached }) { return ("bolt.circle.fill", .blue, String(localized: "キャッシュを利用")) }
    if rs.allSatisfy({ $0.outcome == .reused }) { return ("arrow.triangle.2.circlepath.circle.fill", .teal, String(localized: "再利用")) }
    return ("checkmark.circle.fill", .green, String(localized: "完了"))
  }

  private func seconds(_ g: Group) -> Double? {
    let rs = records(g).compactMap { r in r.start.flatMap { s in r.end.map { Double($0 - s) / 1000 } } }
    return rs.isEmpty ? nil : rs.reduce(0, +)
  }

  @ViewBuilder private func section(_ g: Group) -> some View {
    let (symbol, color, label) = status(g)
    DisclosureGroup(isExpanded: binding(g.id)) {
      VStack(alignment: .leading, spacing: 6) {
        HStack(spacing: 10) {
          ForEach(g.stages) { st in
            let r = stages.last { $0.stage == st }
            HStack(spacing: 3) {
              Image(systemName: st.symbol)
              Text(st.label)
              if let r, let a = r.start, let b = r.end { Text(formatDuration(Double(b - a) / 1000)).foregroundStyle(.secondary) }
              if let r, r.outcome == .cached { Text("cache").foregroundStyle(.blue) }
              if let r, r.outcome == .reused { Text("再利用").foregroundStyle(.teal) }
            }
            .font(.caption)
            .foregroundStyle(r == nil ? .tertiary : .primary)
          }
        }
        switch g.id {
        case "build":
          logText(.build, empty: String(localized: "準備・評価・build の出力はありません（キャッシュや再利用のときは空です）"))
        case "execute":
          Text("stdout").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
          logText(.standardOutput, empty: String(localized: "stdout への出力はありません"))
          Text("stderr").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
          logText(.standardError, empty: String(localized: "stderr への出力はありません"))
        default:
          logText(.journal, empty: String(localized: "記録された出来事はありません"), phases: ["validate", "save"])
          if let failure, label == String(localized: "失敗") { Text(failure).font(.caption).foregroundStyle(.red).textSelection(.enabled) }
        }
      }
      .padding(.leading, 4)
    } label: {
      HStack(spacing: 6) {
        Image(systemName: symbol).foregroundStyle(color)
        Text(g.title).font(.callout.weight(.medium))
        Text(label).font(.caption).foregroundStyle(.secondary)
        Spacer()
        if let s = seconds(g) { Text(formatDuration(s)).font(.caption.monospacedDigit()).foregroundStyle(.secondary) }
      }
    }
    .padding(.horizontal, 8).padding(.vertical, 3)
  }

  private func logText(_ stream: Tsugumi_Data_LogStream, empty: String, phases: [String]? = nil) -> some View {
    let slice = logs[stream]
    let raw = slice?.text ?? ""
    let text = phases.map { journal(raw, for: $0) } ?? raw
    return VStack(alignment: .leading, spacing: 4) {
      if let slice, (slice.offset ?? 0) > 0 {
        HStack {
          Text("ログの末尾を表示しています").font(.caption).foregroundStyle(.secondary)
          Button("ログを書き出す…") { export(stream, size: slice.sizeBytes ?? 0) }
            .disabled(exporting)
        }
        if let exportError { Text(exportError).font(.caption).foregroundStyle(.red) }
      }
      Text(text.isEmpty ? empty : terminalText(text))
        .font(.system(size: AppPreferences.shared.editorFontSize - 1, design: text.isEmpty ? .default : .monospaced))
        .foregroundStyle(text.isEmpty ? .secondary : .primary)
        .textSelection(.enabled)
      if let truncatedAt {
        Text("ログはここで切り詰めました（上限 \(truncatedAt / 1_048_576) MiB）")
          .font(.caption.weight(.semibold))
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
    .padding(6)
    .background(.background.secondary, in: RoundedRectangle(cornerRadius: 5))
  }

  private func export(_ stream: Tsugumi_Data_LogStream, size: UInt64) {
    guard let rpc = session.rpc, let inv = shownInvocation else { return }
    let panel = NSSavePanel()
    panel.nameFieldStringValue = "\(runId)-\(stream).log"
    exporting = true
    exportError = nil
    Task {
      defer { exporting = false }
      guard await panel.begin() == .OK, let url = panel.url else { return }
      do {
        let temporary = try FileManager.default.url(for: .itemReplacementDirectory, in: .userDomainMask,
                                                     appropriateFor: url, create: true)
        defer { try? FileManager.default.removeItem(at: temporary) }
        let file = temporary.appendingPathComponent("log")
        try Data().write(to: file)
        let output = try FileHandle(forWritingTo: file)
        do {
          defer { try? output.close() }
          var offset: UInt64 = 0
          while offset < size {
            let slice = try await rpc.logRange(runId: runId, card: card, invocation: inv, stream: stream,
                                               offset: offset, limit: 256 * 1024)
            guard let next = slice.nextOffset, next > offset else { break }
            try output.write(contentsOf: Data(slice.text.utf8.prefix(Int(min(next, size) - offset))))
            offset = next
          }
        }
        if FileManager.default.fileExists(atPath: url.path) {
          _ = try FileManager.default.replaceItemAt(url, withItemAt: file)
        } else {
          try FileManager.default.moveItem(at: file, to: url)
        }
      } catch {
        exportError = "\(error)"
      }
    }
  }

  /// Journal lines (JSON) of the given phases, as "event: message".
  private func journal(_ text: String, for phases: [String]) -> String {
    text.split(separator: "\n").compactMap { line -> String? in
      guard let v = try? JSONValue.parse(String(line)), let p = v["phase"]?.string, phases.contains(p) else { return nil }
      return "\(p) · \(v["event"]?.string ?? ""): \(v["message"]?.string ?? "")"
    }.joined(separator: "\n")
  }

  private func load() async {
    defer { if !Task.isCancelled { loaded = true } }
    guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
    do {
      var inv = invocation
      if inv == nil, runId == session.currentRunId { inv = session.cardStates[key(card)]?.invocation }
      let rec = try? await rpc.record(runId: runId, card: card, invocation: inv, followLatest: inv == nil)
      inv = inv ?? rec?.selectedInvocation
      var nextStages: [StageRecord] = []
      var nextState: Tsugumi_Data_CardState?
      var nextFailure: String?
      if runId == session.currentRunId, let s = session.cardStates[key(card)], s.invocation == inv {
        nextStages = s.stages
        nextState = s.state
        nextFailure = s.failure
      } else if let summary = rec?.record?.summary {
        nextStages = TimelineEntry.stages(summary.stages)
        nextState = summary.state
        nextFailure = summary.failure?.diagnostic
      }
      var out: [Tsugumi_Data_LogStream: Tsugumi_RPC_LogSliceT] = [:]
      var nextTruncatedAt = rec?.record?.summary.logTruncatedAtBytes
      for stream in [Tsugumi_Data_LogStream.build, .standardOutput, .standardError, .journal] {
        let slice = try await rpc.logRange(runId: runId, card: card, invocation: inv, stream: stream, tail: true)
        guard !Task.isCancelled else { return }
        out[stream] = slice
        nextTruncatedAt = nextTruncatedAt ?? slice.logTruncatedAtBytes
      }
      guard !Task.isCancelled else { return }
      truncatedAt = nextTruncatedAt
      shownInvocation = inv
      stages = nextStages
      state = nextState
      failure = nextFailure
      logs = out
      error = nil
      // A failed stage opens by itself; so does the one asked for.
      if open.isEmpty {
        for g in groups where status(g).2 == String(localized: "失敗") { open.insert(g.id) }
        if let f = focus, let g = groups.first(where: { $0.stages.contains(f) }) { open.insert(g.id) }
      }
      // So does the one running now, to show its output as it comes.
      for g in groups where status(g).2 == String(localized: "進行中") && !openedRunning.contains(g.id) {
        openedRunning.insert(g.id)
        open.insert(g.id)
      }
    } catch let f as RPCFailure where f.kind == .notFound {
      guard !Task.isCancelled else { return }
      shownInvocation = nil
      stages = []
      state = nil
      failure = nil
      logs = [:]
      truncatedAt = nil
      error = nil
    } catch {
      guard !Task.isCancelled else { return }
      self.error = "\(error)"
    }
  }
}
