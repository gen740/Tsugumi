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

  @MainActor init(session: ProjectSession, card: CardKey) {
    runId = session.currentRunId
    state = session.runStatus?.state
    self.card = session.executionBatch(card)?.executionList == true ? nil : session.cardStates[card]
    connected = session.connected
    remoteHistoryRevision = session.remoteHistoryRevision
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

/// Runs of one Card (filter by success/failure) → invocations → I/O, log, info.
struct CardExecutionView: View {
  let session: ProjectSession
  let cardKey: CardKey
  var onOpenSource: (String, Int) -> Void
  @State private var runs: [Tsugumi_RPC_RunSummaryT] = []
  @State private var filter = Filter.all
  @State private var runId: String?
  @State private var invocations: [Tsugumi_RPC_InvocationSummaryT] = []
  @State private var batches: [Tsugumi_RPC_RunBatchT] = []
  @State private var cursors: [String] = []
  @State private var nextCursor: String?
  @State private var batchRevision = 0
  @State private var invocation: UInt64?
  @State private var logStream: Tsugumi_Data_LogStream = .standardOutput
  @State private var record: Tsugumi_RPC_RecordResultT?
  @State private var detail = Detail.io
  @State private var error: String?
  @State private var loaded = false
  @State private var invocationsRunId: String?
  @State private var comparing = false

  enum Filter: String, CaseIterable, Identifiable {
    case all, succeeded, failed
    var id: String { rawValue }
    var title: String { self == .all ? String(localized: "すべて") : (self == .succeeded ? String(localized: "成功") : String(localized: "失敗")) }
  }

  enum Detail: String, CaseIterable, Identifiable {
    case io, log, packages, info
    var id: String { rawValue }
    var title: String {
      switch self {
      case .io: return String(localized: "入出力")
      case .log: return String(localized: "ログ")
      case .packages: return String(localized: "依存 package")
      case .info: return String(localized: "情報")
      }
    }
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
                               description: Text("この Card が実行されると、ここに実行ごとの入出力・ログ・情報が残ります。"))
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
            Button("進行中の実行を表示") { filter = .all; runId = active }.fixedSize()
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
          List(filtered, id: \.status.runId, selection: $runId) { r in
            ExecutionHistoryRunRow(run: r, target: session.displayPath(r.target.path))
            // The selection is optional: the tag must be too, or a click selects nothing.
            .tag(Optional(r.status.runId))
          }
          .overlay {
            if loaded && filtered.isEmpty {
              ContentUnavailableView(runs.isEmpty ? "実行履歴はありません" : "条件に合う実行はありません", systemImage: "clock")
            } else if !loaded { ProgressView() }
          }
        }
      } detail: {
        VStack(spacing: 0) {
          if let mark = runMark(record?.run.status ?? runs.first { $0.status.runId == runId }?.status) { Text(mark).font(.caption).lineLimit(1).padding(6) }
          if runId == nil {
            ContentUnavailableView("実行を選んでください", systemImage: "list.bullet.rectangle")
          } else {
            ExecutionHistoryDetailHeader(detail: $detail) {
              if waitingForInvocation {
                Label("呼び出しの記録を待っています", systemImage: "clock")
                  .lineLimit(1).help("呼び出しの記録を待っています")
              } else {
                ExecutionHistoryInvocationPicker(invocations: invocations, selection: $invocation)
                if let batch = selectedBatch, batch.executionList {
                  Text(verbatim: "×\(batch.invocationCount)").font(.caption.monospaced()).lineLimit(1)
                }
                if !cursors.isEmpty || nextCursor != nil {
                  Button { nextCursor = nil; cursors.removeLast() } label: { Image(systemName: "chevron.left") }
                    .disabled(cursors.isEmpty).help("前へ").accessibilityLabel(Text("前へ"))
                  Button {
                    if let next = nextCursor { nextCursor = nil; cursors.append(next) }
                  } label: { Image(systemName: "chevron.right") }
                    .disabled(nextCursor == nil).help("次へ").accessibilityLabel(Text("次へ"))
                }
              }
            }
            Divider()
            if waitingForInvocation && (detail == .io || detail == .packages) {
              Text("呼び出しが記録されると表示します。")
                .font(.callout).foregroundStyle(.secondary).lineLimit(1)
                .help("呼び出しが記録されると表示します。")
                .padding(10).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            } else {
              switch detail {
              case .io: ioView
              case .log:
                if let runId { StageLogView(session: session, runId: runId, card: path(cardKey), invocation: invocation) }
              case .packages:
                // What this invocation ran with, from its record; never the current definition.
                if record?.exists == false {
                  ContentUnavailableView("この実行では呼び出されていません", systemImage: "circle.dotted")
                } else if let runId {
                  InvocationPackagesView(session: session, runId: runId, environmentId: record?.record?.summary.environmentId ?? "")
                }
              case .info: infoView
              }
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

  @ViewBuilder private var ioView: some View {
    if let rec = record?.record {
      ScrollView {
        VStack(alignment: .leading, spacing: 12) {
          if rec.recordIo == false || record?.recordingDisabled == true {
            Label("この Card は入出力を記録していません（設定 > 入出力 > 入出力を記録）。", systemImage: "info.circle")
              .foregroundStyle(.secondary)
          }
          if let mark = runMark(record?.run.status) { Text(mark).font(.caption) }
          Text("入力").font(.headline)
          if rec.inputs.isEmpty { Text("なし").foregroundStyle(.secondary) }
          ForEach(Array(rec.inputs.compactMap { $0 }.enumerated()), id: \.offset) { _, v in valueBox(v) }
          Text("出力").font(.headline)
          if rec.outputs.isEmpty { Text("なし").foregroundStyle(.secondary) }
          ForEach(Array(rec.outputs.compactMap { $0 }.enumerated()), id: \.offset) { _, v in valueBox(v) }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
      }
    } else if record?.exists == false {
      ContentUnavailableView("この実行では呼び出されていません", systemImage: "circle.dotted")
    } else {
      ContentUnavailableView("記録がありません", systemImage: "tray")
    }
  }

  private func valueBox(_ v: Tsugumi_Data_NamedValueT) -> some View {
    Text(describeValue(v))
      .font(.system(size: 11, design: .monospaced))
      .textSelection(.enabled)
      .padding(8)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(.background.secondary, in: RoundedRectangle(cornerRadius: 6))
  }

  @ViewBuilder private var infoView: some View {
    if let r = record {
      Form {
        LabeledContent("状態", value: r.status.state.label)
        if let s = r.record?.summary ?? invocations.first(where: { $0.invocation == invocation }) {
          LabeledContent("呼び出し", value: "#\(s.invocation ?? 0)")
          LabeledContent("段階", value: String(describing: s.phase))
          LabeledContent("開始", value: formatMillis(s.startedAt))
          LabeledContent("終了", value: formatMillis(s.endedAt))
          if let d = s.durationMs { LabeledContent("所要時間", value: formatDuration(Double(d) / 1000)) }
          if let service = s.executionService, !service.isEmpty {
            LabeledContent("実行サービス", value: service == "local" ? String(localized: "プロジェクトの実行環境") : service)
          }
          if s.executionState != .unspecified { LabeledContent("実行サービスの状態", value: s.executionState.label) }
          if let system = s.executionSystem, !system.isEmpty { LabeledContent("CPU アーキテクチャ", value: system) }
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
            Button("\(cell) · \(source):\(line)") { onOpenSource(source, line) }
          }
        }
        LabeledContent("集計", value: String(localized: "呼び出し \(r.aggregate.calls ?? 0) · 成功 \(r.aggregate.succeeded ?? 0) · 失敗 \(r.aggregate.failed ?? 0)"))
        if let srcs = r.record?.inputSources, !srcs.isEmpty {
          Section("入力元") {
            ForEach(Array(srcs.compactMap { $0 }.enumerated()), id: \.offset) { _, s in
              LabeledContent(s.port, value: "\(session.displayPath(s.card.path)) #\(s.invocation ?? 0)")
            }
          }
        }
        if r.orphaned == true { Text("この Card は文書から削除されています。").foregroundStyle(.secondary) }
      }
      .formStyle(.grouped)
    } else {
      ContentUnavailableView("情報がありません", systemImage: "info.circle")
    }
  }

  private func loadRuns() async {
    guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); loaded = true; return }
    do {
      let page = try await rpc.history(card: path(cardKey), limit: 100).runs.compactMap { $0 }
      guard !Task.isCancelled else { return }
      runs = page
      runId = ExecutionHistorySelection.run(runId, available: runs.compactMap { $0.status.runId },
                                             active: session.runActive ? session.currentRunId : nil)
      error = nil
    } catch {
      guard !Task.isCancelled else { return }
      self.error = String(localized: "履歴を読み込めません: \(String(describing: error))")
    }
    loaded = true
  }

  private func loadInvocations() async {
    guard let rpc = session.rpc, let selectedRun = runId else { invocations = []; return }
    if invocationsRunId != selectedRun {
      invocationsRunId = selectedRun
      invocations = []
      batches = []
      cursors = []
      nextCursor = nil
      invocation = nil
      record = nil
    }
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
    let next = try? await rpc.record(runId: selectedRun, card: path(cardKey), invocation: selectedInvocation, followLatest: selectedInvocation == nil)
    guard !Task.isCancelled, runId == selectedRun, invocation == selectedInvocation else { return }
    record = next
  }
}

struct ExecutionHistoryRunRow: View {
  let run: Tsugumi_RPC_RunSummaryT
  let target: String

  var body: some View {
    HStack {
      Image(systemName: run.status.state.symbol).foregroundStyle(run.status.state.color)
      VStack(alignment: .leading) {
        Text(formatMillis(run.status.acceptedAt)).font(.callout).lineLimit(1)
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

struct ExecutionHistoryInvocationPicker: View {
  let invocations: [Tsugumi_RPC_InvocationSummaryT]
  @Binding var selection: UInt64?

  var body: some View {
    Text("呼び出し").fixedSize().accessibilityHidden(true)
      .anchorPreference(key: ExecutionHistoryControlBounds.self, value: .bounds) { ["label": $0] }
    Picker("呼び出し", selection: $selection) {
      if invocations.isEmpty { Text("呼び出しなし").tag(UInt64?.none) }
      ForEach(invocations, id: \.invocation) { i in
        Text("#\(i.invocation ?? 0) · \(i.state.label)"
             + (i.batchIndex.flatMap { index in i.batchSize.map { " · \(index)/\($0)" } } ?? "")
             + (i.durationMs.map { " · \(formatDuration(Double($0) / 1000))" } ?? ""))
          .lineLimit(1).tag(i.invocation)
      }
    }
    .pickerStyle(.menu).labelsHidden().frame(minWidth: 0, maxWidth: 320).help("呼び出し")
    .anchorPreference(key: ExecutionHistoryControlBounds.self, value: .bounds) { ["invocation": $0] }
  }
}

/// Reserve a row for each control group, then use a menu below the tabs' ideal width.
struct ExecutionHistoryDetailHeader<InvocationControls: View>: View {
  @Binding var detail: CardExecutionView.Detail
  @ViewBuilder var invocationControls: () -> InvocationControls

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      HStack(spacing: 6) { invocationControls() }
        .lineLimit(1)
      ViewThatFits(in: .horizontal) {
        tabs.pickerStyle(.segmented).fixedSize()
        tabs.pickerStyle(.menu)
      }
      .anchorPreference(key: ExecutionHistoryControlBounds.self, value: .bounds) { ["tabs": $0] }
    }
    .padding(6).fixedSize(horizontal: false, vertical: true)
  }

  private var tabs: some View {
    Picker("実行の詳細", selection: $detail) {
      ForEach(CardExecutionView.Detail.allCases) { Text($0.title).lineLimit(1).tag($0) }
    }
    .labelsHidden().help("実行の詳細")
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
struct CardArtifactsView: View {
  let session: ProjectSession
  let cardKey: CardKey
  @State private var runs: [Tsugumi_RPC_RunSummaryT] = []
  @State private var runId: String?
  @State private var artifacts: [Tsugumi_RPC_ArtifactInfoT] = []
  @State private var artifactId: String?
  @State private var member: String?
  @State private var search = ""
  @State private var preview: Preview?
  @State private var error: String?
  @State private var loaded = false

  enum Preview {
    case html(String)
    case text(String)
    case image(NSImage)
    case binary(Int)
  }

  var body: some View {
    Group {
      if !loaded {
        ProgressView("成果物を読み込んでいます…").frame(maxWidth: .infinity, maxHeight: .infinity)
      } else if runs.isEmpty, let error {
        ContentUnavailableView {
          Label("成果物を読み込めませんでした", systemImage: "exclamationmark.triangle")
        } description: { Text(error) } actions: { Button("再試行") { Task { await loadRuns() } } }
      } else if runs.isEmpty {
        let retains = session.cardSetting(cardKey, "retainArtifact")?.bool == true
        ContentUnavailableView("保持された成果物はありません", systemImage: "shippingbox",
                               description: Text(retains ? "成果物を保持する設定です。実行すると、ここに残ります。"
                                                         : "この Card は成果物を保持していません（設定 > 入出力 > 成果物を保持）。"))
      } else {
        content
      }
    }
    .navigationTitle(session.displayName(cardKey))
    .task(id: session.lastFinished) { await loadRuns() }
  }

  private var content: some View {
    VStack(spacing: 0) {
      HStack {
        Picker("実行", selection: $runId) {
          if runs.isEmpty { Text("成果物のある実行はありません").tag(String?.none) }
          ForEach(runs, id: \.status.runId) { r in
            Text("\(formatMillis(r.status.acceptedAt)) · \(r.status.state.label)" + (runMark(r.status).map { " · " + $0 } ?? "")).tag(Optional(r.status.runId))
          }
        }
        .frame(maxWidth: 360)
        if let mark = runMark(runs.first { $0.status.runId == runId }?.status) { Text(mark).font(.caption) }
        Spacer()
        if let a = selectedArtifact, let m = member {
          Button("書き出す…") { Task { await export(a, m) } }
        }
      }
      .padding(6)
      Divider()
      HSplitView {
        List(artifacts, id: \.artifactId, selection: $artifactId) { a in
          VStack(alignment: .leading) {
            Text(a.output).font(.callout.bold())
            Text("#\(a.invocation ?? 0) · \(String(describing: a.kind)) · \(a.members.count) 件").font(.caption).foregroundStyle(.secondary)
          }
          .tag(Optional(a.artifactId))
        }
        .overlay {
          if loaded && artifacts.isEmpty {
            ContentUnavailableView("成果物はありません", systemImage: "shippingbox",
                                   description: Text("設定 > 入出力 で「成果物を保持」と保持する出力を選ぶと残ります。"))
          }
        }
        .frame(minWidth: 180, idealWidth: 220)
        VStack(spacing: 0) {
          TextField("ファイルを検索", text: $search).textFieldStyle(.roundedBorder).padding(6)
          List(members, id: \.path, selection: $member) { m in
            HStack {
              Image(systemName: "doc")
              Text(m.path).lineLimit(1)
              Spacer()
              Text(ByteCountFormatter.string(fromByteCount: Int64(m.sizeBytes ?? 0), countStyle: .file)).font(.caption).foregroundStyle(.secondary)
            }
            .tag(Optional(m.path))
          }
        }
        .frame(minWidth: 220, idealWidth: 280)
        Group {
          switch preview {
          case .html(let html)?:
            ArtifactHTMLPreview(html: html)
          case .text(let t)?:
            ScrollView { Text(t).font(.system(size: 11, design: .monospaced)).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading).padding(8) }
          case .image(let i)?:
            Image(nsImage: i).resizable().scaledToFit().padding()
          case .binary(let n)?:
            ContentUnavailableView("プレビューできません", systemImage: "doc.questionmark", description: Text("バイナリ \(n) bytes。書き出して確認してください。"))
          case nil:
            ContentUnavailableView(member == nil ? "ファイルを選んでください" : "読み込み中…", systemImage: "eye")
          }
        }
        .frame(minWidth: 260)
      }
      if let error { Text(error).font(.caption).foregroundStyle(.red).padding(6) }
    }
    .task(id: runId) { await loadArtifacts() }
    .task(id: "\(artifactId ?? "")|\(member ?? "")") { await loadPreview() }
  }

  private var selectedArtifact: Tsugumi_RPC_ArtifactInfoT? { artifacts.first { $0.artifactId == artifactId } }

  private var members: [Tsugumi_RPC_ArtifactMemberT] {
    (selectedArtifact?.members.compactMap { $0 } ?? []).filter { search.isEmpty || $0.path.localizedCaseInsensitiveContains(search) }
  }

  private func loadRuns() async {
    defer { loaded = true }
    guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
    do {
      runs = try await rpc.history(card: path(cardKey), artifactsOnly: true, limit: 100).runs.compactMap { $0 }
      if runId == nil { runId = runs.first?.status.runId }
      error = nil
    } catch { self.error = String(localized: "成果物を読み込めません: ") + "\(error)" }
  }

  private func loadArtifacts() async {
    guard let rpc = session.rpc, let runId else { artifacts = []; return }
    artifacts = (try? await rpc.artifacts(runId: runId, card: path(cardKey))) ?? []
    artifactId = artifacts.first?.artifactId
    member = selectedArtifact?.members.first??.path
  }

  private func loadPreview() async {
    preview = nil
    guard let rpc = session.rpc, let a = selectedArtifact, let member else { return }
    do {
      let data = try await rpc.readArtifact(id: a.artifactId, member: a.kind == .directory ? member : nil)
      guard !Task.isCancelled else { return }
      if let html = ArtifactHTMLPreview.text(member: member, data: data) {
        preview = .html(html)
      } else if let img = NSImage(data: data), ["png", "jpg", "jpeg", "gif", "tiff", "pdf", "svg"].contains((member as NSString).pathExtension.lowercased()) {
        preview = .image(img)
      } else if let s = String(data: data.prefix(256 * 1024), encoding: .utf8) {
        preview = .text(s + (data.count > 256 * 1024 ? String(localized: "\n…（先頭 256 KiB を表示）") : ""))
      } else {
        preview = .binary(data.count)
      }
      error = nil
    } catch { self.error = "\(error)" }
  }

  /// Exports through ArtifactRead; the core never receives a macOS path.
  private func export(_ a: Tsugumi_RPC_ArtifactInfoT, _ m: String) async {
    guard let rpc = session.rpc else { return }
    let p = NSSavePanel()
    p.nameFieldStringValue = (m as NSString).lastPathComponent
    guard p.runModal() == .OK, let url = p.url else { return }
    do {
      try await rpc.readArtifact(id: a.artifactId, member: a.kind == .directory ? m : nil).write(to: url)
    } catch { self.error = "\(error)" }
  }
}

/// A Card's output display enlarged in its own window.
struct VisualizationWindow: View {
  let session: ProjectSession
  let cardKey: CardKey

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack {
        Text(session.displayName(cardKey)).font(.title2.bold())
        if let d = session.displays[cardKey] { Text("呼び出し #\(d.invocation)").foregroundStyle(.secondary) }
        Spacer()
        Button("チャットに参照を追加") { AppModel.shared.chat(for: session).addReference(.display(cardKey)) }
      }
      DisplayContent(display: session.displays[cardKey], large: true)
    }
    .padding(16)
    .navigationTitle(session.displayName(cardKey))
    .task { await session.refreshVisibleDisplays() }
  }
}
