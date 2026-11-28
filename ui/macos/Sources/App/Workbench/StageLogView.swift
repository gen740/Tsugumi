import AppKit
import SwiftUI
import TsugumiRPC

/// The log of one invocation of a Card, like a CI step log: a pipeline line
/// with each stage's state and time, then one foldable log with a section per
/// group of stages (preparation, Nix evaluation and build / execution /
/// validation and save). Execution shows the Card's output: what it wrote to
/// stdout and stderr, as one stream in the order it wrote. Only open sections
/// are read, each from its end and then as it grows; a closed section is its
/// header only. Nothing here waits on the core before drawing.
struct StageLogView: View {
  let session: ProjectSession
  let runId: String
  let card: [String]
  let invocation: UInt64?
  /// A stage to open (a click on a stage icon).
  var focus: Stage?
  /// The invocation's record (stages and state of a past run, the latest call).
  @State private var info: Tsugumi_RPC_RecordResultT?
  @State private var infoError: String?
  /// The reader's own fold choices (true: closed), kept while the view is open.
  @State private var folded: [String: Bool] = [:]
  @State private var readers = ["build": LogStreamReader(), "execute": LogStreamReader(),
                                "save": LogStreamReader(form: .journal)]
  @State private var exporting = false
  @State private var exportError: String?
  /// An open section has been loading for a moment.
  @State private var slow = false

  private struct Refresh: Equatable {
    let runId: String
    let invocation: UInt64?
    let card: [String]
    let execution: ExecutionHistoryRefresh
  }

  struct Group: Identifiable {
    let id: String
    let title: String
    let stages: [Stage]
    let stream: Tsugumi_Data_LogStream
  }

  private var groups: [Group] {
    let code = session.kind(key(card)) == .code
    return (code ? [Group(id: "build", title: String(localized: "準備・Nix 評価・ビルド"), stages: [.prepare, .evaluate, .build], stream: .build)] : []) + [
      Group(id: "execute", title: String(localized: "実行"), stages: [.execute], stream: .output),
      Group(id: "save", title: String(localized: "出力検証・保存"), stages: [.validate, .save], stream: .journal),
    ]
  }

  private var live: CardRunState? {
    guard runId == session.currentRunId, let s = session.cardStates[key(card)],
          invocation == nil || s.invocation == invocation else { return nil }
    return s
  }

  /// The invocation shown: the one asked for, else the latest one.
  private var shownInvocation: UInt64? { invocation ?? live?.invocation ?? info?.selectedInvocation }

  private var stages: [StageRecord] {
    if let live { return live.stages }
    return info?.record.map { TimelineEntry.stages($0.summary.stages) } ?? []
  }

  private var state: Tsugumi_Data_CardState? { live?.state ?? info?.record?.summary.state }
  private var failure: String? { live?.failure ?? info?.record?.summary.failure?.plainText }
  private var service: String? {
    let name = live?.executionService ?? info?.record?.summary.executionService
    return name?.isEmpty == false ? name : nil
  }

  /// What a remote execution waits for or does now (live only).
  private var activity: ExecutionActivityText? { live.flatMap(ExecutionActivityText.init) }

  private func isOpen(_ g: Group) -> Bool {
    if let closed = folded[g.id] { return !closed }
    func busy(_ g: Group) -> Bool { [SectionStatus.running, .waiting, .failed].contains(status(g)) }
    // Running, waiting and failed sections open; with none of them, the
    // execution's output is what the reader came for.
    return busy(g) || (g.id == "execute" && !groups.contains(where: busy))
  }

  private func source(_ g: Group) -> LogStreamReader.Source? {
    guard let inv = shownInvocation else { return nil }
    return LogStreamReader.Source(runId: runId, card: card, invocation: inv, stream: g.stream,
                                  live: session.runActive && runId == session.currentRunId && g.stream == .output,
                                  recordingRevision: session.recordingRevision)
  }

  /// What each open section reads; closed ones read nothing.
  private var shownSources: [String: LogStreamReader.Source?] {
    Dictionary(uniqueKeysWithValues: groups.map { ($0.id, isOpen($0) ? source($0) : nil) })
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      HStack {
        Text(String(localized: "\(session.displayName(key(card))) · \(shownInvocation ?? 0) 回目") + (state.map { " · " + $0.label } ?? ""))
          .font(.callout.weight(.medium)).lineLimit(1)
        Spacer()
        let all = groups.allSatisfy(isOpen)
        Button(all ? String(localized: "すべて閉じる") : String(localized: "すべて開く")) {
          for g in groups { folded[g.id] = all }
        }
        .controlSize(.small)
        if shownInvocation != nil {
          Button("ログを書き出す…") { export() }.controlSize(.small).disabled(exporting)
        }
      }
      .padding(.horizontal, 8).padding(.vertical, 5)
      pipeline
      Divider()
      if let infoError, info == nil && live == nil {
        ContentUnavailableView {
          Label("ログを読み込めませんでした", systemImage: "exclamationmark.triangle")
        } description: { Text(infoError) } actions: { Button("再試行") { Task { await loadInfo() } } }
      } else if info?.exists == false && live == nil {
        ContentUnavailableView("この実行では、この Card は呼び出されていません", systemImage: "circle.dotted")
      } else {
        if let e = exportError { Text(e).font(.caption).foregroundStyle(.red).padding(.horizontal, 8) }
        VStack(alignment: .leading, spacing: 0) {
          ForEach(groups) { g in section(g) }
          Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
      }
    }
    .onChange(of: "\(runId)|\(key(card))|\(invocation ?? 0)") {
      info = nil
      infoError = nil
      folded = [:]
      exportError = nil
      open(focus)
    }
    // State changes (a stage starts or ends) read the record again; new
    // output is read as LogAvailable announces it.
    .task(id: Refresh(runId: runId, invocation: invocation, card: card,
                      execution: ExecutionHistoryRefresh(session: session, card: key(card)))) {
      await loadInfo()
    }
    .onChange(of: shownSources, initial: true) { _, sources in
      for g in groups {
        readers[g.id]?.show(sources[g.id] ?? nil, form: g.stream == .journal ? .journal : .terminal, session: session)
      }
    }
    .onChange(of: session.connected) { readers.values.forEach { $0.update() } }
    .onChange(of: state) { readers.values.forEach { $0.update() } }
    .onChange(of: focus, initial: true) { open(focus) }
    // A section still loading shows nothing; after a moment, a quiet word that it loads.
    .task(id: loading) {
      slow = false
      guard loading else { return }
      try? await Task.sleep(for: .milliseconds(300))
      if !Task.isCancelled { slow = true }
    }
  }

  private func open(_ stage: Stage?) {
    guard let stage else { return }
    let parts = stage.parts
    guard let g = groups.first(where: { g in parts.contains(where: g.stages.contains) }) else { return }
    folded[g.id] = false
  }

  // MARK: Pipeline

  /// The stages as the Card's face draws them, and what the running one does.
  private var pipeline: some View {
    TimelineView(.periodic(from: .now, by: 1)) { _ in
      StagePipelineView(line: line) { open($0) }
    }
  }

  /// The pipeline line now; the sections take their state and time from it.
  private var line: StagePipeline {
    StagePipeline(stages: groups.flatMap(\.stages), records: stages, service: service,
                  live: live, now: session.coreNow(),
                  lastActivity: live?.executionLastActivity ?? info?.record?.summary.executionLastActivity,
                  cpuMs: live.map(\.cpuMs) ?? info?.record?.summary.cpuMs,
                  buildCached: live.map { $0.cached ? true : nil } ?? info?.record?.summary.buildCached)
  }

  static func stageLabel(_ st: Stage) -> String {
    switch st {
    case .prepare: return String(localized: "準備")
    case .evaluate: return String(localized: "Nix 評価")
    case .build: return "build"
    case .execute: return "run"
    case .validate: return "validate"
    case .save: return String(localized: "保存")
    default: return st.label
    }
  }

  // MARK: Sections

  @ViewBuilder private func section(_ g: Group) -> some View {
    let open = isOpen(g)
    let part = line.section(g.stages, ended: !ongoing)
    let st = SectionStatus(part.status, waiting: part.waiting)
    Button { folded[g.id] = open } label: {
      HStack(spacing: 6) {
        Image(systemName: "chevron.right").font(.caption2.weight(.semibold)).foregroundStyle(.secondary)
          .rotationEffect(.degrees(open ? 90 : 0)).frame(width: 10)
        StatusSymbol(status: st)
        Text(g.title)
        if g.id == "execute", let service { Text("(\(service))").foregroundStyle(.secondary) }
        if part.cached { Text("cache").font(.caption).foregroundStyle(StagePalette.reuse) }
        if part.reused { Text("再利用").font(.caption).foregroundStyle(StagePalette.reuse) }
        Spacer()
        TimelineView(.periodic(from: .now, by: 1)) { _ in
          let now = line.section(g.stages, ended: !ongoing)
          Text(summary(SectionStatus(now.status, waiting: now.waiting), seconds: now.seconds))
            .font(.caption).foregroundStyle(.secondary).monospacedDigit()
        }
      }
      .font(.callout).lineLimit(1)
      .padding(.horizontal, 8).padding(.vertical, 5)
      .background(open ? Color.secondary.opacity(0.08) : .clear)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .accessibilityLabel(Text(g.title + " · " + st.label))
    .accessibilityValue(Text(open ? String(localized: "開いています") : String(localized: "閉じています")))
    if open, let reader = readers[g.id] {
      if let e = reader.error { Text(e).font(.caption).foregroundStyle(.red).padding(.horizontal, 8) }
      // As tall as its text, up to the room there is; the room left goes
      // below the last section.
      LogTextView(reader: reader, placeholder: placeholder(g, reader), fitsText: true)
        .frame(maxWidth: .infinity)
        .padding(.leading, 18)
        .layoutPriority(1)
      // At the end of the section's stream, where the output stops.
      if let limit = reader.truncatedAt {
        Text("ログはここで切り詰めました（上限 \(limit) バイト）")
          .font(.caption.weight(.semibold)).padding(.horizontal, 26).padding(.vertical, 2)
      }
    }
    if st == .failed, let failure {
      Text(failure).font(.caption).foregroundStyle(.red).textSelection(.enabled).lineLimit(6)
        .padding(.leading, 26).padding(.trailing, 8).padding(.vertical, 4)
    }
    Divider()
  }

  private var ongoing: Bool {
    guard let state else { return false }
    return [.waiting, .ready, .preparing, .running].contains(state)
  }

  /// A section header's words: its time (the sum of its stages' times) and state.
  private func summary(_ st: SectionStatus, seconds: Double?) -> String {
    switch st {
    case .waiting: return activity?.text ?? st.label
    case .running: return [seconds.map(formatDuration), st.label].compactMap { $0 }.joined(separator: " · ")
    case .pending: return ongoing ? String(localized: "まだ") : String(localized: "実行されていません")
    default: return seconds.map(formatDuration) ?? st.label
    }
  }

  private func placeholder(_ g: Group, _ reader: LogStreamReader) -> String {
    Self.sectionPlaceholder(g.id, known: info != nil || live != nil || invocation != nil, invocation: shownInvocation,
                            loaded: reader.loaded || reader.error != nil,
                            waiting: activity.flatMap { $0.waiting ? $0.text : nil }, slow: slow)
  }

  /// An open section still reading its stream.
  private var loading: Bool {
    groups.contains { g in isOpen(g) && readers[g.id].map { !$0.loaded && $0.error == nil } == true }
  }

  /// What an open log section says while it has no text. Only a stream that
  /// finished loading empty says it is empty; while it loads the section
  /// shows nothing, and after a moment (`slow`) a quiet word that it loads.
  /// `known`: the invocation's record or live state is in; `invocation`: the
  /// one shown (nil: the Card has not run in this run); `waiting`: what a
  /// waiting execution waits for. The same rule as the Web UI's
  /// sectionPlaceholder.
  static func sectionPlaceholder(_ section: String, known: Bool, invocation: UInt64?, loaded: Bool,
                                 waiting: String?, slow: Bool) -> String {
    if known && invocation == nil { return String(localized: "この実行では、この Card はまだ動いていません") }
    if !known || !loaded { return slow ? String(localized: "読み込んでいます…") : "" }
    switch section {
    case "build": return String(localized: "準備・評価・build の出力はありません（キャッシュや再利用のときは空です）")
    case "save": return String(localized: "記録された出来事はありません")
    default: return waiting ?? String(localized: "出力はありません")
    }
  }

  /// A section's state by the pipeline line's rule (StagePipeline.section).
  private func status(_ g: Group) -> SectionStatus {
    let part = line.section(g.stages, ended: !ongoing)
    return SectionStatus(part.status, waiting: part.waiting)
  }

  // MARK: Export

  /// Writes the invocation's whole log, section by section, to one file.
  private func export() {
    let parts = groups.compactMap { g in source(g).map { (title: g.title, source: $0) } }
    guard let first = parts.first?.1 else { return }
    exporting = true
    exportError = nil
    Task {
      defer { exporting = false }
      do {
        try await exportLog(session, parts, name: "\(first.runId)-\(session.displayName(key(card)))-\(first.invocation ?? 0).log")
      } catch { exportError = "\(error)" }
    }
  }

  private func loadInfo() async {
    guard let rpc = session.rpc else { infoError = String(localized: "実行環境に接続していません"); return }
    do {
      let rec = try await rpc.record(runId: runId, card: card, invocation: invocation, followLatest: invocation == nil)
      guard !Task.isCancelled else { return }
      info = rec
      infoError = nil
    } catch let f as RPCFailure where f.kind == .notFound {
      guard !Task.isCancelled else { return }
      info = nil
      infoError = nil
    } catch {
      guard !Task.isCancelled else { return }
      infoError = "\(error)"
    }
  }
}

/// The state of a stage or a log section as the log view shows it.
/// The Card's stage strip on the canvas uses the same states and symbols.
enum SectionStatus: Equatable {
  case pending, waiting, running, done, cached, reused, failed, cancelled

  init(_ status: StageProgress.Status, waiting: Bool = false) {
    switch status {
    case .pending, .notRun: self = .pending
    case .running: self = waiting ? .waiting : .running
    case .done: self = .done
    case .cached: self = .cached
    case .reused: self = .reused
    case .failed: self = .failed
    case .cancelled: self = .cancelled
    }
  }

  var label: String {
    switch self {
    case .pending: return String(localized: "未開始")
    case .waiting: return String(localized: "待機中")
    case .running: return String(localized: "進行中")
    case .done: return String(localized: "完了")
    case .cached: return String(localized: "キャッシュを利用")
    case .reused: return String(localized: "再利用")
    case .failed: return String(localized: "失敗")
    case .cancelled: return String(localized: "停止")
    }
  }

  /// The stage circle's status that draws it (waiting is a running circle that waits).
  var stage: StageProgress.Status {
    switch self {
    case .pending: return .pending
    case .waiting, .running: return .running
    case .done: return .done
    case .cached: return .cached
    case .reused: return .reused
    case .failed: return .failed
    case .cancelled: return .cancelled
    }
  }
}

/// A section's state as the Card's stage circle draws it (StageDot): the
/// same shapes and palette as the Card's face, the pipeline line and the
/// hover's stage table. `size` is the circle's diameter.
struct StatusSymbol: View {
  let status: SectionStatus
  var size: CGFloat = 11

  var body: some View {
    StageDot(status: status.stage, size: size, waiting: status == .waiting)
      .accessibilityLabel(Text(status.label))
  }
}
