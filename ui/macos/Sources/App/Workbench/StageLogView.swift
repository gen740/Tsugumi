import AppKit
import SwiftUI
import TsugumiRPC

/// The log of one invocation of a Card by stage, like a CI job: SDK
/// preparation, Nix evaluation and build / execution / validation and save,
/// each with its state and duration; the chosen one's output fills the rest.
/// The raw streams stay available under "By stream". Only the shown stream
/// is read, from its end and then as it grows; nothing here waits on the
/// core before drawing.
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
  @State private var selected: String?
  /// The reader picked a group: a group starting to run no longer takes over.
  @State private var chosen = false
  @State private var rawStream: Tsugumi_Data_LogStream = .standardOutput
  @State private var executeStream: Tsugumi_Data_LogStream = .standardOutput
  @State private var reader = LogStreamReader()
  @State private var exporting = false
  @State private var exportError: String?

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

  /// The stream the chosen group shows, and how.
  private var shownStream: (Tsugumi_Data_LogStream, journal: Bool) {
    switch currentGroup {
    case "build": return (.build, false)
    case "execute": return (executeStream, false)
    case "save": return (.journal, true)
    default: return (rawStream, false)
    }
  }

  private var currentGroup: String { selected ?? defaultGroup }

  /// Running, else failed, else the one asked for, else the execution.
  private var defaultGroup: String {
    if let g = groups.first(where: { status($0).2 == String(localized: "進行中") }) { return g.id }
    if let g = groups.first(where: { status($0).2 == String(localized: "失敗") }) { return g.id }
    if let f = focus, let g = groups.first(where: { $0.stages.contains(f) }) { return g.id }
    return "execute"
  }

  private struct VisibleLog: Equatable {
    let source: LogStreamReader.Source?
    let form: LogForm
  }

  private var visibleLog: VisibleLog { VisibleLog(source: source, form: shownStream.journal ? .journal : .terminal) }

  private var source: LogStreamReader.Source? {
    guard let inv = shownInvocation else { return nil }
    return LogStreamReader.Source(runId: runId, card: card, invocation: inv, stream: shownStream.0,
                           live: session.runActive && runId == session.currentRunId &&
                             (shownStream.0 == .standardOutput || shownStream.0 == .standardError),
                           recordingRevision: session.recordingRevision)
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      HStack {
        Text(String(localized: "\(session.displayName(key(card))) · \(shownInvocation ?? 0) 回目") + (state.map { " · " + $0.label } ?? ""))
          .font(.callout.weight(.medium)).lineLimit(1)
        Spacer()
        if reader.hasBytes, let source {
          Button("ログを書き出す…") { export(source, size: reader.size) }.controlSize(.small).disabled(exporting)
        }
      }
      .padding(.horizontal, 8).padding(.vertical, 5)
      groupBar
      Divider()
      if let infoError, info == nil && live == nil {
        ContentUnavailableView {
          Label("ログを読み込めませんでした", systemImage: "exclamationmark.triangle")
        } description: { Text(infoError) } actions: { Button("再試行") { Task { await loadInfo() } } }
      } else if info?.exists == false && live == nil {
        ContentUnavailableView("この実行では、この Card は呼び出されていません", systemImage: "circle.dotted")
      } else {
        streamBar
        notices
        LogTextView(reader: reader, placeholder: placeholder)
          .frame(maxWidth: .infinity, maxHeight: .infinity)
        // At the end of the shown stream, where the output stops.
        if let limit = reader.truncatedAt {
          Text("ログはここで切り詰めました（上限 \(limit) バイト）")
            .font(.caption.weight(.semibold)).padding(.horizontal, 8).padding(.vertical, 2)
        }
        if let failure, status(groups.first { $0.id == currentGroup }).2 == String(localized: "失敗") {
          Text(failure).font(.caption).foregroundStyle(.red).textSelection(.enabled).lineLimit(6)
            .padding(.horizontal, 8).padding(.vertical, 4)
        }
      }
    }
    .onChange(of: "\(runId)|\(key(card))|\(invocation ?? 0)") {
      info = nil
      infoError = nil
      selected = nil
      chosen = false
      exportError = nil
    }
    // State changes (a stage starts or ends) read the record again; new
    // output is read as LogAvailable announces it.
    .task(id: Refresh(runId: runId, invocation: invocation, card: card,
                      execution: ExecutionHistoryRefresh(session: session, card: key(card)))) {
      await loadInfo()
    }
    .onChange(of: visibleLog, initial: true) { _, log in
      reader.show(log.source, form: log.form, session: session)
    }
    .onChange(of: session.connected) { reader.update() }
    .onChange(of: state) { reader.update() }
    .onChange(of: focus) { if let f = focus, let g = groups.first(where: { $0.stages.contains(f) }) { selected = g.id; chosen = true } }
    .onChange(of: defaultGroup) { if !chosen { selected = nil } }
  }

  private var groupBar: some View {
    ScrollView(.horizontal, showsIndicators: false) {
      HStack(spacing: 6) {
        ForEach(groups) { g in
          let (symbol, color, label) = status(g)
          chip(id: g.id, help: g.title + " · " + label) {
            Image(systemName: symbol).foregroundStyle(color)
            Text(g.title)
            if let s = seconds(g) { Text(formatDuration(s)).foregroundStyle(.secondary).monospacedDigit() }
          }
        }
        chip(id: "raw", help: String(localized: "stream 別（元の出力）")) {
          Image(systemName: "list.bullet.rectangle")
          Text("stream 別")
        }
      }
      .padding(.horizontal, 8).padding(.bottom, 6)
    }
    .fixedSize(horizontal: false, vertical: true)
  }

  private func chip<Label: View>(id: String, help: String, @ViewBuilder label: () -> Label) -> some View {
    Button { selected = id; chosen = true } label: {
      HStack(spacing: 4) { label() }.font(.caption).lineLimit(1)
        .padding(.horizontal, 8).padding(.vertical, 4)
        .background(currentGroup == id ? Color.accentColor.opacity(0.18) : Color.secondary.opacity(0.08),
                    in: RoundedRectangle(cornerRadius: 6))
    }
    .buttonStyle(.plain).help(help).accessibilityLabel(Text(help))
    .accessibilityAddTraits(currentGroup == id ? .isSelected : [])
  }

  /// The chosen group's stages with their times, and its stream when it has several.
  @ViewBuilder private var streamBar: some View {
    HStack(spacing: 10) {
      switch currentGroup {
      case "execute":
        Picker("", selection: $executeStream) {
          Text("stdout").tag(Tsugumi_Data_LogStream.standardOutput)
          Text("stderr").tag(Tsugumi_Data_LogStream.standardError)
        }
        .pickerStyle(.segmented).labelsHidden().fixedSize()
      case "raw":
        Picker("", selection: $rawStream) {
          Text("stdout").tag(Tsugumi_Data_LogStream.standardOutput)
          Text("stderr").tag(Tsugumi_Data_LogStream.standardError)
          Text("build").tag(Tsugumi_Data_LogStream.build)
          Text("journal").tag(Tsugumi_Data_LogStream.journal)
        }
        .pickerStyle(.segmented).labelsHidden().fixedSize()
      default:
        EmptyView()
      }
      if let g = groups.first(where: { $0.id == currentGroup }) {
        ForEach(g.stages) { st in
          let r = stages.last { $0.stage == st }
          HStack(spacing: 3) {
            Image(systemName: st.symbol)
            Text(st.label)
            if let r, let a = r.start, let b = r.end { Text(formatDuration(Double(b - a) / 1000)).foregroundStyle(.secondary) }
            if let r, r.outcome == .cached { Text("cache").foregroundStyle(.blue) }
            if let r, r.outcome == .reused { Text("再利用").foregroundStyle(.teal) }
          }
          .font(.caption).lineLimit(1)
          .foregroundStyle(r == nil ? .tertiary : .primary)
        }
      }
      Spacer(minLength: 0)
    }
    .padding(.horizontal, 8).padding(.vertical, 4)
  }

  @ViewBuilder private var notices: some View {
    if let e = reader.error ?? exportError { Text(e).font(.caption).foregroundStyle(.red).padding(.horizontal, 8) }
  }

  private var placeholder: String {
    if shownInvocation == nil || (!reader.loaded && reader.error == nil) {
      return info == nil && live == nil ? String(localized: "読み込んでいます…") : String(localized: "この実行では、この Card はまだ動いていません")
    }
    switch currentGroup {
    case "build": return String(localized: "準備・評価・build の出力はありません（キャッシュや再利用のときは空です）")
    case "save": return String(localized: "記録された出来事はありません")
    case "execute":
      return executeStream == .standardError ? String(localized: "stderr への出力はありません") : String(localized: "stdout への出力はありません")
    default: return String(localized: "この stream への出力はありません")
    }
  }

  private func records(_ g: Group) -> [StageRecord] { stages.filter { g.stages.contains($0.stage) } }

  private func status(_ g: Group?) -> (String, Color, String) {
    guard let g else { return ("circle.dashed", .secondary, "") }
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

  private func export(_ source: LogStreamReader.Source, size: UInt64) {
    exporting = true
    exportError = nil
    Task {
      defer { exporting = false }
      do { try await exportLog(session, source, size: size) } catch { exportError = "\(error)" }
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
