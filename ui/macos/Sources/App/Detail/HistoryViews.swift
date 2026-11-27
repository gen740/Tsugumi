import AppKit
import SwiftUI
import TsugumiRPC

/// Refresh history on state transitions even when a build prints no log bytes.
struct ExecutionHistoryRefresh: Equatable {
  let runId: String?
  let state: Tsugumi_Data_RunState?
  let card: CardRunState?
  let connected: Bool
  let remoteHistoryRevision: Int
  let recordingRevision: Int

  @MainActor init(session: ProjectSession, card: CardKey) {
    runId = session.currentRunId
    state = session.runStatus?.state
    self.card = session.executionBatch(card)?.executionList == true ? nil : session.cardStates[card]
    connected = session.connected
    remoteHistoryRevision = session.remoteHistoryRevision
    recordingRevision = session.recordingRevision
  }
}

/// Keep an explicit historical selection; a newly opened view prefers the live run.
enum ExecutionHistorySelection {
  static func run(_ selected: String?, available: [String], active: String?) -> String? {
    if let selected, available.contains(selected) { return selected }
    if let active, available.contains(active) { return active }
    return available.first
  }

  static func invocation(_ selected: UInt64?, previous: [UInt64], available: [UInt64]) -> UInt64? {
    // Follow new calls only while the reader was already following the latest one.
    if let selected, selected != previous.last, available.contains(selected) { return selected }
    return available.last
  }
}

/// Runs of one Card (filter by success/failure) → invocations → what one
/// invocation was (information first) and what it took and gave. Its log is
/// in the Log category; its environment comes from the Card's definition
/// (Dependencies).
struct CardExecutionView: View {
  let session: ProjectSession
  let model: CardEditorModel
  private var cardKey: CardKey { model.cardKey }
  @State private var runs: [Tsugumi_RPC_RunSummaryT] = []
  @State private var filter = Filter.all
  @State private var runId: String?
  @State private var invocations: [Tsugumi_RPC_InvocationSummaryT] = []
  @State private var batches: [Tsugumi_RPC_RunBatchT] = []
  @State private var cursors: [String] = []
  @State private var nextCursor: String?
  @State private var batchRevision = 0
  @State private var invocation: UInt64?
  @State private var record: Tsugumi_RPC_RecordResultT?
  @State private var recordError: String?
  @State private var error: String?
  @State private var loaded = false
  @State private var comparing = false

  enum Filter: String, CaseIterable, Identifiable {
    case all, succeeded, failed
    var id: String { rawValue }
    var title: String { self == .all ? String(localized: "すべて") : (self == .succeeded ? String(localized: "成功") : String(localized: "失敗")) }
  }

  /// Loading, failed and empty are said plainly, never as an empty split view.
  var body: some View {
    Group {
      if !loaded {
        ProgressView("履歴を読み込んでいます…").frame(maxWidth: .infinity, maxHeight: .infinity)
      } else if runs.isEmpty, let error {
        ContentUnavailableView {
          Label("履歴を読み込めませんでした", systemImage: "exclamationmark.triangle")
        } description: { Text(error) } actions: { Button("再試行") { Task { await loadRuns() } } }
      } else if runs.isEmpty {
        ContentUnavailableView("まだ実行していません", systemImage: "clock",
                               description: Text("この Card が実行されると、ここに実行ごとの情報と入出力が残ります。"))
      } else {
        content
      }
    }
    .navigationTitle(session.displayName(cardKey))
    .task(id: ExecutionHistoryRefresh(session: session, card: cardKey)) {
      await loadRuns()
    }
    .task { await observeBatchHistory(session) { batchRevision = $0 } }
    .sheet(isPresented: $comparing) { RunComparisonView(session: session, runs: runs, initial: runId) { comparing = false } }
  }

  private struct DetailRefresh: Equatable {
    let runId: String?
    let invocation: UInt64?
    let execution: ExecutionHistoryRefresh
    var cursor: String? = nil
    var batchRevision = 0
  }

  private var content: some View {
    VStack(spacing: 0) {
      if session.runActive, let active = session.currentRunId {
        HStack {
          Label(runId == active ? "進行中の実行を表示しています" : "過去の実行を表示しています。別の実行が進行中です。",
                systemImage: runId == active ? "play.circle" : "clock.arrow.circlepath")
            .help(runId == active ? "進行中の実行を表示しています" : "過去の実行を表示しています。別の実行が進行中です。")
          Spacer()
          if runId != active, runs.contains(where: { $0.status.runId == active }) {
            Button("進行中の実行を表示") { filter = .all; selectRun(active) }.fixedSize()
          }
        }
        .font(.callout).lineLimit(1).padding(8).fixedSize(horizontal: false, vertical: true)
        Divider()
      }
      ExecutionHistoryPanes {
        VStack(spacing: 0) {
          Picker("", selection: $filter) { ForEach(Filter.allCases) { Text($0.title).tag($0) } }
            .pickerStyle(.segmented).labelsHidden().padding(6)
          if runs.count > 1 {
            HStack {
              Spacer()
              Button("実行を比較") { comparing = true }.fixedSize()
            }.padding(6).fixedSize(horizontal: false, vertical: true)
          }
          List(selection: Binding(get: { runId }, set: selectRun)) {
            ForEach(filtered, id: \.status.runId) { r in
              if let id = r.status.runId {
                Button { selectRun(id) } label: {
                  ExecutionHistoryRunRow(run: r, target: session.displayPath(r.target.path))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .tag(id)
              }
            }
          }
          .overlay {
            if loaded && filtered.isEmpty {
              ContentUnavailableView(runs.isEmpty ? "実行履歴はありません" : "条件に合う実行はありません", systemImage: "clock")
            } else if !loaded { ProgressView() }
          }
        }
      } detail: {
        VStack(spacing: 0) {
          if runId == nil {
            ContentUnavailableView("実行を選んでください", systemImage: "list.bullet.rectangle")
          } else {
            ExecutionHistoryDetailHeader {
              if waitingForInvocation {
                Label("呼び出しの記録を待っています", systemImage: "clock")
                  .lineLimit(1).help("呼び出しの記録を待っています")
              } else {
                ExecutionHistoryInvocationPicker(invocations: invocations, selection: $invocation,
                                                 paged: !cursors.isEmpty || nextCursor != nil)
                if let batch = selectedBatch, batch.executionList {
                  Text(verbatim: "×\(batch.invocationCount)").font(.caption.monospaced()).lineLimit(1)
                    .help("実行リストの要素数")
                }
                if !cursors.isEmpty || nextCursor != nil {
                  Button { nextCursor = nil; cursors.removeLast() } label: { Image(systemName: "chevron.left") }
                    .disabled(cursors.isEmpty).help("前の 500 回").accessibilityLabel(Text("前へ"))
                  Button {
                    if let next = nextCursor { nextCursor = nil; cursors.append(next) }
                  } label: { Image(systemName: "chevron.right") }
                    .disabled(nextCursor == nil).help("次の 500 回").accessibilityLabel(Text("次へ"))
                }
              }
            } trailing: {
              if let runId {
                DeleteRunCategory(session: session, runId: runId)
                Button("ログ") { model.openLog(runId: runId, invocation: invocation) }
                  .disabled(record?.exists == false).help("この呼び出しのログを「ログ」で開きます")
              }
            }
            Divider()
            if waitingForInvocation {
              Text("呼び出しが記録されると表示します。")
                .font(.callout).foregroundStyle(.secondary).lineLimit(1)
                .help("呼び出しが記録されると表示します。")
                .padding(10).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            } else if let recordError {
              ContentUnavailableView("履歴を読み込めませんでした", systemImage: "exclamationmark.triangle",
                                     description: Text(recordError))
            } else if record == nil {
              ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if record?.exists == false {
              ContentUnavailableView("この実行では呼び出されていません", systemImage: "circle.dotted",
                                     description: Text("実行の対象に含まれなかったか、入力がそろう前に実行が終わりました。"))
            } else {
              detailForm
            }
          }
          if let error { Text(error).font(.caption).foregroundStyle(.red).padding(6) }
        }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
    .task(id: DetailRefresh(runId: runId, invocation: nil, execution: ExecutionHistoryRefresh(session: session, card: cardKey), cursor: cursors.last, batchRevision: batchRevision)) {
      await loadInvocations()
    }
    .task(id: DetailRefresh(runId: runId, invocation: invocation, execution: ExecutionHistoryRefresh(session: session, card: cardKey), batchRevision: batchRevision)) {
      await loadRecord()
    }
  }

  private var waitingForInvocation: Bool {
    session.runActive && runId == session.currentRunId && invocations.isEmpty && record?.record == nil
  }

  private var selectedBatch: Tsugumi_RPC_RunBatchT? {
    if let id = invocations.first(where: { $0.invocation == invocation })?.batchActivation {
      return batches.first { $0.activation == id }
    }
    return batches.last
  }

  private var filtered: [Tsugumi_RPC_RunSummaryT] {
    runs.filter { r in
      switch filter {
      case .all: return true
      case .succeeded: return r.status.state == .succeeded
      case .failed: return r.status.state == .failed || r.status.state == .cancelled
      }
    }
  }

  /// Not recorded is a setting of this Card, said as such: the section stays,
  /// greyed, instead of an empty one that looks lost.
  private var ioRecorded: Bool {
    !(record?.recordingDisabled == true || record?.record?.recordIo == false)
  }

  /// Information first, then inputs and outputs, in one scrolling form.
  @ViewBuilder private var detailForm: some View {
    if let r = record {
      Form {
        infoSection(r)
        if !ioRecorded, r.record?.inputs.isEmpty != false, r.record?.outputs.isEmpty != false {
          Section {
            Label("この Card は入出力を記録しない設定です。値は残っていません。", systemImage: "nosign")
              .foregroundStyle(.secondary)
            Button("入出力の設定を開く") { model.category = .ports }
          } header: {
            Text("入出力（記録していません）").foregroundStyle(.tertiary)
          }
        } else if let rec = r.record {
          Section("入力") {
            if rec.inputs.isEmpty { Text("なし").foregroundStyle(.secondary) }
            ForEach(Array(rec.inputs.compactMap { $0 }.enumerated()), id: \.offset) { _, v in valueBox(v) }
          }
          Section("出力") {
            if rec.outputs.isEmpty { Text("なし").foregroundStyle(.secondary) }
            ForEach(Array(rec.outputs.compactMap { $0 }.enumerated()), id: \.offset) { _, v in valueBox(v) }
          }
        } else {
          Section("入出力") { Text("この呼び出しの記録はまだありません。").foregroundStyle(.secondary) }
        }
      }
      .formStyle(.grouped)
    } else {
      ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
    }
  }

  private func valueBox(_ v: Tsugumi_Data_NamedValueT) -> some View {
    Text(describeValue(v))
      .font(.system(size: 11, design: .monospaced))
      .textSelection(.enabled)
      .frame(maxWidth: .infinity, alignment: .leading)
  }

  @ViewBuilder private func infoSection(_ r: Tsugumi_RPC_RecordResultT) -> some View {
    Section("情報") {
      LabeledContent("状態", value: r.status.state.label)
      if let mark = runMark(r.run.status) { LabeledContent("実行の種類", value: mark) }
      if let s = r.record?.summary ?? invocations.first(where: { $0.invocation == invocation }) {
        LabeledContent("呼び出し", value: invocationOrdinal(s, total: r.aggregate.calls))
        LabeledContent("段階", value: String(describing: s.phase))
        LabeledContent("開始", value: formatMillis(s.startedAt))
        LabeledContent("終了", value: formatMillis(s.endedAt))
        if let d = s.durationMs { LabeledContent("所要時間", value: formatDuration(Double(d) / 1000)) }
        if let service = s.executionService, !service.isEmpty {
          LabeledContent("実行サービス", value: service == "local" ? String(localized: "プロジェクトの実行環境") : service)
        }
        ForEach(Array((r.record?.workerProbes ?? []).compactMap { $0 }.enumerated()), id: \.offset) { _, probe in
          let name = probe.worker == "local" ? String(localized: "プロジェクトの実行環境") : probe.worker
          let score = probe.score.map { $0.formatted() }
          LabeledContent(name, value: probe.probe == nil && score != nil
                         ? String(localized: "計測なし（同順位）")
                         : score.map { "\(String(localized: "スコア")) \($0)" }
                           ?? probe.unavailableReason ?? probe.failure ?? String(localized: "計測に失敗しました"))
          if let output = probe.stdoutText, !output.isEmpty {
            LabeledContent("計測の標準出力") { Text(output).textSelection(.enabled) }
          }
          if let output = probe.stderrText, !output.isEmpty {
            LabeledContent("計測の標準エラー") { Text(output).textSelection(.enabled) }
          }
        }
        if s.executionState != .unspecified { LabeledContent("実行サービスの状態", value: s.executionState.label) }
        if let system = s.executionSystem, !system.isEmpty { LabeledContent("CPU アーキテクチャ", value: system) }
        if let cached = s.buildCached {
          LabeledContent("ビルド出力の取得元", value: s.buildSubstituter
                         ?? (cached ? String(localized: "既存の Nix ストア") : String(localized: "ビルドで生成")))
        }
        if let pushed = s.buildPushed {
          LabeledContent("ビルド出力のキャッシュ送信",
                         value: pushed ? String(localized: "送信済み") : String(localized: "未送信"))
        }
        if let job = s.executionJobId, !job.isEmpty {
          LabeledContent("ジョブ ID", value: job).textSelection(.enabled)
          if r.run.status.failure?.kind == .interrupted {
            Text("Tsugumi の再起動により、この実行は中断されました。結果は回収されません。必要な Card を改めて実行してください。")
              .font(.caption).foregroundStyle(.secondary)
          }
        }
        if let cpu = s.executionResources?.cpuMillis { LabeledContent("CPU 上限（ミリコア）", value: String(cpu)) }
        if let memory = s.executionResources?.memoryBytes { LabeledContent("メモリ上限（バイト）", value: String(memory)) }
        // A Card built for another system (its `system` setting): what ran it.
        if let e = s.emulator, !e.isEmpty { LabeledContent("エミュレーター", value: e).textSelection(.enabled) }
        // Card secret names handed to the process; their values are never recorded.
        let secrets = s.secrets.compactMap { $0 }
        if !secrets.isEmpty { LabeledContent("渡したシークレット", value: secrets.joined(separator: ", ")).textSelection(.enabled) }
        if !s.waitingInputs.isEmpty {
          LabeledContent("待っていた入力", value: s.waitingInputs.compactMap { $0?.port }.joined(separator: ", "))
        }
      }
      if let f = r.status.failure {
        LabeledContent("失敗の種類", value: String(describing: f.kind))
        Text(f.diagnostic ?? "").textSelection(.enabled).foregroundStyle(.red)
        let args = f.args.compactMap { $0 }
        if let source = args.first(where: { $0.key == "source" })?.value,
           let rawLine = args.first(where: { $0.key == "line" })?.value,
           let line = Int(rawLine), line > 0 {
          let cell = args.first(where: { $0.key == "cell" })?.value ?? source
          Button("\(cell) · \(source):\(line)") { model.openSource(source, line: line) }
        }
      }
      LabeledContent("この実行での集計", value: String(localized: "呼び出し \(r.aggregate.calls ?? 0) · 成功 \(r.aggregate.succeeded ?? 0) · 失敗 \(r.aggregate.failed ?? 0)"))
      if let srcs = r.record?.inputSources, !srcs.isEmpty {
        ForEach(Array(srcs.compactMap { $0 }.enumerated()), id: \.offset) { _, s in
          LabeledContent(String(localized: "入力元 \(s.port)"), value: "\(session.displayPath(s.card.path)) · \(String(localized: "\(s.invocation ?? 0) 回目"))")
        }
      }
      if r.orphaned == true { Text("この Card は文書から削除されています。").foregroundStyle(.secondary) }
    }
  }

  private func invocationOrdinal(_ s: Tsugumi_RPC_InvocationSummaryT, total: UInt64?) -> String {
    let n = s.invocation ?? 0
    guard let total, total > 1 else { return String(localized: "\(n) 回目（この実行で 1 回だけ）") }
    return String(localized: "\(n) 回目（この実行で \(total) 回）")
  }

  private func loadRuns() async {
    guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); loaded = true; return }
    do {
      let page = try await rpc.history(card: path(cardKey), limit: 100).runs.compactMap { $0 }
      guard !Task.isCancelled else { return }
      runs = page
      selectRun(ExecutionHistorySelection.run(runId, available: runs.compactMap { $0.status.runId },
                                             active: session.runActive ? session.currentRunId : nil))
      error = nil
    } catch {
      guard !Task.isCancelled else { return }
      self.error = String(localized: "履歴を読み込めません: \(String(describing: error))")
    }
    loaded = true
  }

  private func selectRun(_ id: String?) {
    guard runId != id else { return }
    runId = id
    invocations = []
    batches = []
    cursors = []
    nextCursor = nil
    invocation = nil
    record = nil
    recordError = nil
  }

  private func loadInvocations() async {
    guard let rpc = session.rpc, let selectedRun = runId else { return }
    let cursor = cursors.last
    let result = try? await rpc.invocations(runId: selectedRun, card: path(cardKey), cursor: cursor)
    guard !Task.isCancelled, runId == selectedRun, cursor == cursors.last else { return }
    let page = result?.records.compactMap { $0 } ?? []
    batches = result?.batches.compactMap { $0 } ?? []
    nextCursor = result?.nextCursor
    invocation = ExecutionHistorySelection.invocation(invocation, previous: invocations.compactMap { $0.invocation },
                                                       available: page.compactMap { $0.invocation })
    invocations = page
  }

  private func loadRecord() async {
    guard let rpc = session.rpc, let selectedRun = runId else { record = nil; return }
    let selectedInvocation = invocation
    do {
      let next = try await rpc.record(runId: selectedRun, card: path(cardKey), invocation: selectedInvocation,
                                      followLatest: selectedInvocation == nil)
      guard !Task.isCancelled, runId == selectedRun, invocation == selectedInvocation else { return }
      record = next
      recordError = nil
    } catch {
      guard !Task.isCancelled, runId == selectedRun, invocation == selectedInvocation else { return }
      record = nil
      recordError = String(describing: error)
    }
  }
}

struct ExecutionHistoryRunRow: View {
  let run: Tsugumi_RPC_RunSummaryT
  let target: String

  var body: some View {
    HStack {
      Image(systemName: run.status.state.symbol).foregroundStyle(run.status.state.color)
      VStack(alignment: .leading) {
        if let id = run.status.runId {
          Text("実行 ID \(id)").font(.callout.monospacedDigit().weight(.medium)).lineLimit(1)
        }
        Text(formatMillis(run.status.acceptedAt)).font(.caption).foregroundStyle(.secondary).lineLimit(1)
        if let mark = runMark(run.status) { Text(mark).font(.caption).lineLimit(1) }
        Text("\(run.status.state.label) · 対象 \(target)")
          .font(.caption).foregroundStyle(.secondary).lineLimit(1).help(target)
      }
    }
  }
}

/// The Card sidebar consumes 161 pt; the remaining pane must also fit at 620 pt.
struct ExecutionHistoryPanes<Runs: View, Detail: View>: View {
  @ViewBuilder var runs: () -> Runs
  @ViewBuilder var detail: () -> Detail

  var body: some View {
    GeometryReader { geometry in
      if geometry.size.width >= 640 {
        HSplitView {
          runs().frame(minWidth: 220, idealWidth: 240, maxWidth: 280, maxHeight: .infinity)
          detail().frame(minWidth: 320, maxWidth: .infinity, maxHeight: .infinity)
        }
      } else {
        VStack(spacing: 0) {
          runs().frame(height: min(160, geometry.size.height * 0.35))
          Divider()
          detail().frame(maxWidth: .infinity, maxHeight: .infinity)
        }
      }
    }
  }
}

/// Which call of the Card this is. A Card is called once each time its inputs
/// are all there (once per element of an execution list), so most runs have
/// one call: that is said, with no menu of one item.
struct ExecutionHistoryInvocationPicker: View {
  let invocations: [Tsugumi_RPC_InvocationSummaryT]
  @Binding var selection: UInt64?
  /// More calls than one page (500) shows.
  var paged = false

  static let explanation = String(localized: "Card は入力がそろうたびに 1 回呼び出されます（実行リストなら要素ごと）。ほかの実行の呼び出しは左の一覧で実行を選びます。")

  var body: some View {
    Text("呼び出し").fixedSize().accessibilityHidden(true)
      .anchorPreference(key: ExecutionHistoryControlBounds.self, value: .bounds) { ["label": $0] }
    Group {
      if invocations.count <= 1 && !paged {
        Text(invocations.first.map { Self.label($0) + " · " + String(localized: "この実行で 1 回だけ") } ?? String(localized: "呼び出しなし"))
          .lineLimit(1).truncationMode(.middle)
          .frame(minWidth: 0, maxWidth: 320, alignment: .leading)
      } else {
        Picker("呼び出し", selection: $selection) {
          ForEach(invocations, id: \.invocation) { i in Text(Self.label(i)).lineLimit(1).tag(i.invocation) }
        }
        .pickerStyle(.menu).labelsHidden().frame(minWidth: 0, maxWidth: 320)
        Text(paged ? String(localized: "500 回ずつ") : String(localized: "全 \(invocations.count) 回"))
          .font(.caption).foregroundStyle(.secondary).lineLimit(1).fixedSize()
      }
    }
    .help(Self.explanation)
    .anchorPreference(key: ExecutionHistoryControlBounds.self, value: .bounds) { ["invocation": $0] }
  }

  static func label(_ i: Tsugumi_RPC_InvocationSummaryT) -> String {
    String(localized: "\(i.invocation ?? 0) 回目") + " · " + i.state.label
      // A call of an execution list says which element it took.
      + (i.batchIndex.flatMap { index in i.batchSize.flatMap { $0 > 1 ? " · " + String(localized: "実行リスト \(index)/\($0)") : nil } } ?? "")
      // A named implementation (run list label or swept values) says which one it ran.
      + (i.implementationLabel.flatMap { $0.isEmpty ? nil : " · " + $0 } ?? "")
      + (i.durationMs.map { " · \(formatDuration(Double($0) / 1000))" } ?? "")
  }
}

/// The invocation controls on one row, with room kept for the trailing actions.
struct ExecutionHistoryDetailHeader<InvocationControls: View, Trailing: View>: View {
  @ViewBuilder var invocationControls: () -> InvocationControls
  @ViewBuilder var trailing: () -> Trailing

  var body: some View {
    HStack(spacing: 6) {
      invocationControls()
      Spacer(minLength: 4)
      trailing()
    }
    .lineLimit(1)
    .padding(6).fixedSize(horizontal: false, vertical: true)
  }
}

/// Control bounds let offscreen layout tests inspect SwiftUI and AppKit pickers alike.
struct ExecutionHistoryControlBounds: PreferenceKey {
  static var defaultValue: [String: Anchor<CGRect>] { [:] }
  static func reduce(value: inout [String: Anchor<CGRect>], nextValue: () -> [String: Anchor<CGRect>]) {
    value.merge(nextValue(), uniquingKeysWith: { _, next in next })
  }
}

/// Retained outputs of one Card: run/invocation → outputs → files → preview.
private struct DeleteRunCategory: View {
  let session: ProjectSession
  let runId: String
  private struct Selection {
    let runId: String
    let category: Tsugumi_RPC_RunRecordCategory
    let label: String
  }
  @State private var selection: Selection?
  @State private var confirming = false
  @State private var busy = false
  @State private var error: String?

  var body: some View {
    Menu("記録を削除…") {
      Button("標準出力と標準エラー") { prepare(.logs, String(localized: "標準出力と標準エラー")) }
      Button("値") { prepare(.values, String(localized: "値")) }
      Button("成果物") { prepare(.artifacts, String(localized: "成果物")) }
    }
    .disabled(busy || session.runActive || !session.connected)
    .confirmationDialog(
      Text("実行 \(selection?.runId ?? "") の「\(selection?.label ?? "")」を削除しますか？"),
      isPresented: $confirming, titleVisibility: .visible, presenting: selection
    ) { selection in
      Button("削除", role: .destructive) {
        busy = true
        Task {
          defer { busy = false }
          do { try await session.deleteRunCategory(selection.runId, selection.category) }
          catch let failure { error = String(describing: failure) }
        }
      }
      Button("キャンセル", role: .cancel) {}
    } message: { selection in
      if selection.category == .artifacts {
        Text("この実行が作った成果物の中身を削除します。値に含まれる成果物の ID は利用できなくなります。元に戻せません。")
      }
    }
    if let error {
      Text("削除できませんでした: \(error)").font(.caption).foregroundStyle(.red).textSelection(.enabled)
    }
  }

  private func prepare(_ category: Tsugumi_RPC_RunRecordCategory, _ label: String) {
    error = nil
    selection = Selection(runId: runId, category: category, label: label)
    confirming = true
  }
}

struct CardArtifactsView: View {
  let session: ProjectSession
  let cardKey: CardKey

  @State private var runs: [String] = []
  @State private var currentRun: String?
  @State private var cursor: String?
  @State private var loaded = false
  @State private var loadingMore = false
  @State private var error: String?

  var body: some View {
    Group {
      if !loaded {
        ProgressView("Artifacts を読み込んでいます…")
      } else if runs.isEmpty {
        if let error {
          ContentUnavailableView {
            Label("Artifacts を読み込めませんでした", systemImage: "exclamationmark.triangle")
          } description: { Text(error) } actions: { Button("再試行") { Task { await loadRuns() } } }
        } else {
          ContentUnavailableView("この Card の Artifacts はありません", systemImage: "shippingbox")
        }
      } else {
        VStack(spacing: 0) {
          HStack {
            Picker("実行", selection: $currentRun) {
              ForEach(runs, id: \.self) { id in Text("実行 ID \(id)").tag(id) }
            }
            .fixedSize()
            if let cursor {
              Button("さらに表示") { Task { await loadRuns(after: cursor) } }.disabled(loadingMore)
            }
            Spacer()
          }
          .padding(8)
          Divider()
          if let currentRun {
            ArtifactFilesView(session: session, cardKey: cardKey, runId: currentRun).id(currentRun)
          }
          if let error { Text(error).font(.caption).foregroundStyle(.red).padding(6) }
        }
      }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .navigationTitle(session.displayName(cardKey))
    .task(id: ExecutionHistoryRefresh(session: session, card: cardKey)) { await loadRuns() }
  }

  private func loadRuns(after previous: String? = nil) async {
    guard let rpc = session.rpc else {
      error = String(localized: "実行環境に接続していません")
      loaded = true
      return
    }
    if previous != nil { loadingMore = true }
    defer { if previous != nil { loadingMore = false } }
    do {
      // Refresh the current results without discarding the loaded pages.
      let limit = UInt32(previous == nil ? min(500, max(50, runs.count)) : 50)
      let page = try await rpc.history(card: path(cardKey), artifactsOnly: true, cursor: previous, limit: limit)
      guard !Task.isCancelled, previous == nil || previous == cursor else { return }
      let ids = page.runs.compactMap { $0?.status.runId }
      runs = previous == nil ? ids : runs + ids.filter { !runs.contains($0) }
      cursor = page.nextCursor
      if currentRun.flatMap({ runs.contains($0) ? $0 : nil }) == nil { currentRun = runs.first }
      error = nil
    } catch {
      guard !Task.isCancelled else { return }
      self.error = String(describing: error)
    }
    loaded = true
  }
}

/// A browser of the host's sealed artifacts, with each file relative to its artifact.
private struct ArtifactFilesView: View {
  let session: ProjectSession
  let cardKey: CardKey
  let runId: String

  @State private var artifacts: [Tsugumi_RPC_ArtifactInfoT] = []
  @State private var loaded = false
  @State private var error: String?
  @State private var exporting = false

  var body: some View {
    let byCall = Dictionary(grouping: artifacts) { $0.invocation ?? 0 }
    let multipleCalls = artifacts.contains { ($0.invocationCount ?? 0) > 1 }
    VStack(spacing: 0) {
      List {
        ForEach(byCall.keys.sorted(), id: \.self) { invocation in
          if multipleCalls && invocation > 0 {
            Text("\(invocation) 回目")
              .font(.headline)
              .listRowSeparator(.hidden)
              .accessibilityAddTraits(.isHeader)
          }
          ForEach(byCall[invocation] ?? [], id: \.artifactId) { artifact in
            Section {
              let members = artifact.members.compactMap { $0 }
              if members.isEmpty {
                Text("ファイルはありません").foregroundStyle(.secondary)
              }
              ForEach(members, id: \.path) { member in
                fileRow(member, artifactID: artifact.artifactId)
              }
            } header: {
              artifactHeading(artifact)
            }
          }
        }
      }
      .overlay {
        if !loaded { ProgressView() }
        else if artifacts.isEmpty && error == nil {
          ContentUnavailableView("この実行の Artifacts にファイルはありません", systemImage: "shippingbox")
        }
      }
      if let error {
        Text(error).font(.caption).foregroundStyle(.red).textSelection(.enabled).padding(8)
      }
    }
    .task(id: ExecutionHistoryRefresh(session: session, card: cardKey)) { await loadFiles() }
  }

  private func artifactHeading(_ artifact: Tsugumi_RPC_ArtifactInfoT) -> some View {
    let outputs = artifact.outputs.compactMap { output in output.map { $0.port + $0.valuePath } }
    return HStack(spacing: 8) {
      if outputs.isEmpty {
        Text("対応する出力の記録なし")
      } else {
        Text(verbatim: outputs.joined(separator: ", ")).font(.body.monospaced())
      }
      if let label = artifact.label {
        Text(verbatim: label).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
      }
    }
    .help(artifact.relativeLocation ?? "")
  }

  private func fileRow(_ member: Tsugumi_RPC_ArtifactMemberT, artifactID: String) -> some View {
    HStack {
      Image(systemName: "doc").foregroundStyle(.secondary)
      Text(verbatim: member.path).lineLimit(1).truncationMode(.middle).help(member.path)
      Spacer()
      Text(ByteCountFormatter.string(fromByteCount: Int64(clamping: member.sizeBytes ?? 0), countStyle: .file))
        .font(.caption).foregroundStyle(.secondary)
      Button { Task { await download(artifactID, member: member.path) } } label: { Image(systemName: "arrow.down.to.line") }
        .buttonStyle(.borderless)
        .disabled(exporting)
        .help("コピーを保存…")
        .accessibilityLabel("コピーを保存…")
    }
  }

  private func loadFiles() async {
    guard let rpc = session.rpc else {
      error = String(localized: "実行環境に接続していません")
      loaded = true
      return
    }
    do {
      let result = try await rpc.artifacts(runId: runId, card: path(cardKey))
      guard !Task.isCancelled else { return }
      artifacts = result
      error = nil
    } catch {
      guard !Task.isCancelled else { return }
      self.error = error.localizedDescription
    }
    loaded = true
  }

  private func download(_ artifact: String, member: String) async {
    guard let rpc = session.rpc, !exporting else { return }
    exporting = true
    defer { exporting = false }
    let panel = NSSavePanel()
    panel.nameFieldStringValue = (member as NSString).lastPathComponent
    guard await panel.begin() == .OK, let destination = panel.url else { return }
    do {
      try await rpc.readArtifact(id: artifact, member: member).write(to: destination, options: .atomic)
    } catch { self.error = error.localizedDescription }
  }
}
