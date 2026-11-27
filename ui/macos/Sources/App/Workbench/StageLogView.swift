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
  private var failure: String? { live?.failure ?? info?.record?.summary.failure?.diagnostic }
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
          if !groups.contains(where: isOpen) { Spacer(minLength: 0) }
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
  }

  private func open(_ stage: Stage?) {
    guard let stage else { return }
    let parts = stage.parts
    guard let g = groups.first(where: { g in parts.contains(where: g.stages.contains) }) else { return }
    folded[g.id] = false
  }

  // MARK: Pipeline

  /// The stages in order joined by arrows, each with its state and time.
  private var pipeline: some View {
    TimelineView(.periodic(from: .now, by: 1)) { _ in
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 6) {
          let shown = groups.flatMap(\.stages)
          ForEach(Array(shown.enumerated()), id: \.element) { index, st in
            if index > 0 { Text("→").foregroundStyle(.tertiary) }
            pipelineStage(st)
          }
        }
        .font(.caption).lineLimit(1)
        .padding(.horizontal, 8).padding(.vertical, 6)
      }
      .fixedSize(horizontal: false, vertical: true)
    }
  }

  private func pipelineStage(_ st: Stage) -> some View {
    let r = stages.last { $0.stage == st }
    let shownStatus = SectionStatus(r?.outcome, waiting: st == .execute && activity?.waiting == true)
    let current = shownStatus == .running || shownStatus == .waiting
    return Button { open(st) } label: {
      HStack(spacing: 4) {
        StatusSymbol(status: shownStatus)
        Text(shownStatus == .waiting ? String(localized: "待機中") : Self.stageLabel(st))
          .fontWeight(current ? .semibold : .regular)
          .foregroundStyle(r == nil ? Color.secondary : current ? Color.orange : Color.primary)
        if st == .execute, let service { Text("· " + service).foregroundStyle(current ? Color.orange : Color.secondary) }
        if st == .execute, current, let activity { Text("· " + activity.text).foregroundStyle(.secondary) }
        if st == .execute, current, let live, let text = session.livenessText(live) { Text("· " + text).foregroundStyle(.secondary) }
        if let time = duration(r) { Text(time).foregroundStyle(.secondary).monospacedDigit() }
        if r?.outcome == .cached { Text("· cache").foregroundStyle(.secondary) }
        if r?.outcome == .reused { Text("· " + String(localized: "再利用")).foregroundStyle(.secondary) }
      }
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .help(Self.stageLabel(st) + " · " + shownStatus.label)
  }

  static func stageLabel(_ st: Stage) -> String {
    switch st {
    case .prepare: return String(localized: "準備")
    case .evaluate: return String(localized: "Nix 評価")
    case .build: return String(localized: "ビルド")
    case .execute: return String(localized: "実行")
    case .validate: return String(localized: "出力検証")
    case .save: return String(localized: "保存")
    default: return st.label
    }
  }

  /// A stage's time: its duration, or how long it has run so far.
  private func duration(_ r: StageRecord?) -> String? {
    guard let r, let start = r.start else { return nil }
    if let end = r.end { return formatDuration(Double(end - start) / 1000) }
    guard r.outcome == .running else { return nil }
    return formatDuration(Double(max(0, session.coreNow() - start)) / 1000)
  }

  // MARK: Sections

  @ViewBuilder private func section(_ g: Group) -> some View {
    let open = isOpen(g)
    let st = status(g)
    Button { folded[g.id] = open } label: {
      HStack(spacing: 6) {
        Image(systemName: "chevron.right").font(.caption2.weight(.semibold)).foregroundStyle(.secondary)
          .rotationEffect(.degrees(open ? 90 : 0)).frame(width: 10)
        StatusSymbol(status: st)
        Text(g.title)
        if g.id == "execute", let service { Text("(\(service))").foregroundStyle(.secondary) }
        if records(g).contains(where: { $0.outcome == .cached }) { Text("cache").font(.caption).foregroundStyle(.blue) }
        if records(g).contains(where: { $0.outcome == .reused }) { Text("再利用").font(.caption).foregroundStyle(.teal) }
        Spacer()
        TimelineView(.periodic(from: .now, by: 1)) { _ in
          Text(summary(g, st)).font(.caption).foregroundStyle(.secondary).monospacedDigit()
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
      LogTextView(reader: reader, placeholder: placeholder(g, reader))
        .frame(maxWidth: .infinity, minHeight: 60, maxHeight: .infinity)
        .padding(.leading, 18)
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

  private func summary(_ g: Group, _ st: SectionStatus) -> String {
    let rs = records(g)
    switch st {
    case .waiting: return activity?.text ?? st.label
    case .running:
      let time = rs.compactMap(\.start).min().map { formatDuration(Double(max(0, session.coreNow() - $0)) / 1000) }
      return [time, st.label].compactMap { $0 }.joined(separator: " · ")
    case .pending: return ongoing ? String(localized: "まだ") : String(localized: "実行されていません")
    default:
      let total = rs.compactMap { r in r.start.flatMap { s in r.end.map { Double($0 - s) / 1000 } } }
      return total.isEmpty ? st.label : formatDuration(total.reduce(0, +))
    }
  }

  private func placeholder(_ g: Group, _ reader: LogStreamReader) -> String {
    if shownInvocation == nil || (!reader.loaded && reader.error == nil) {
      return info == nil && live == nil ? String(localized: "読み込んでいます…") : String(localized: "この実行では、この Card はまだ動いていません")
    }
    switch g.id {
    case "build": return String(localized: "準備・評価・build の出力はありません（キャッシュや再利用のときは空です）")
    case "save": return String(localized: "記録された出来事はありません")
    default:
      if let activity, activity.waiting { return activity.text }
      return String(localized: "出力はありません")
    }
  }

  private func records(_ g: Group) -> [StageRecord] { stages.filter { g.stages.contains($0.stage) } }

  private func status(_ g: Group) -> SectionStatus {
    let rs = records(g)
    if rs.contains(where: { $0.outcome == .failed }) { return .failed }
    if rs.contains(where: { $0.outcome == .cancelled }) { return .cancelled }
    if rs.contains(where: { $0.outcome == .running }) {
      return g.stages.contains(.execute) && activity?.waiting == true ? .waiting : .running
    }
    if rs.isEmpty { return .pending }
    if rs.allSatisfy({ $0.outcome == .cached }) { return .cached }
    if rs.allSatisfy({ $0.outcome == .reused }) { return .reused }
    return .done
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

  init(_ outcome: Tsugumi_Data_StageOutcome?, waiting: Bool = false) {
    switch outcome {
    case .running?: self = waiting ? .waiting : .running
    case .failed?: self = .failed
    case .cancelled?: self = .cancelled
    case .cached?: self = .cached
    case .reused?: self = .reused
    case nil, .unspecified?: self = .pending
    default: self = .done
    }
  }

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
}

/// A stage's state as a plain symbol, never shaped like a control: a check,
/// a spinning indicator, an hourglass, a thin empty circle, a cross.
/// Cached and reused stages are a check in blue and teal.
struct StatusSymbol: View {
  let status: SectionStatus
  var size: CGFloat = 14

  var body: some View {
    Group {
      switch status {
      case .running: ProgressView().controlSize(.mini).scaleEffect(size / 16)
      case .waiting: Image(systemName: "hourglass").foregroundStyle(.orange)
      case .done: Image(systemName: "checkmark").foregroundStyle(.green)
      case .cached: Image(systemName: "checkmark").foregroundStyle(.blue)
      case .reused: Image(systemName: "checkmark").foregroundStyle(.teal)
      case .failed: Image(systemName: "xmark").foregroundStyle(.red)
      case .cancelled: Image(systemName: "minus").foregroundStyle(.brown)
      case .pending:
        Image(systemName: "circle").font(.system(size: size * 0.6, weight: .light)).foregroundStyle(.secondary)
      }
    }
    .font(.system(size: size * 0.8, weight: .bold))
    .frame(width: size, height: size)
    .accessibilityLabel(Text(status.label))
  }
}
