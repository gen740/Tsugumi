import AppKit
import SwiftUI
import TsugumiRPC

/// Runs of one Card (filter by success/failure) → invocations → I/O, log, info.
struct CardExecutionView: View {
  let session: ProjectSession
  let cardKey: CardKey
  @State private var runs: [Tsugumi_RPC_RunSummaryT] = []
  @State private var filter = Filter.all
  @State private var runId: String?
  @State private var invocations: [Tsugumi_RPC_InvocationSummaryT] = []
  @State private var invocation: UInt64?
  @State private var logStream: Tsugumi_Data_LogStream = .stdout
  @State private var record: Tsugumi_RPC_RecordResultT?
  @State private var detail = Detail.io
  @State private var error: String?
  @State private var loaded = false

  enum Filter: String, CaseIterable, Identifiable {
    case all, succeeded, failed
    var id: String { rawValue }
    var title: String { self == .all ? String(localized: "すべて") : (self == .succeeded ? String(localized: "成功") : String(localized: "失敗")) }
  }

  enum Detail: String, CaseIterable, Identifiable {
    case io, log, info
    var id: String { rawValue }
    var title: String { self == .io ? String(localized: "入出力") : (self == .log ? String(localized: "ログ") : String(localized: "情報")) }
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
    .task(id: session.lastFinished) { await loadRuns() }
  }

  private var content: some View {
    HSplitView {
      VStack(spacing: 0) {
        Picker("", selection: $filter) { ForEach(Filter.allCases) { Text($0.title).tag($0) } }
          .pickerStyle(.segmented).labelsHidden().padding(6)
        List(filtered, id: \.status.runId, selection: $runId) { r in
          HStack {
            Image(systemName: r.status.state.symbol).foregroundStyle(r.status.state.color)
            VStack(alignment: .leading) {
              Text(formatMillis(r.status.acceptedAt)).font(.callout)
              Text("\(r.status.state.label) · 対象 \(r.target.path.joined(separator: "/"))").font(.caption).foregroundStyle(.secondary)
            }
          }
          // The selection is optional: the tag must be too, or a click selects nothing.
          .tag(Optional(r.status.runId))
        }
        .overlay {
          if loaded && filtered.isEmpty {
            ContentUnavailableView(runs.isEmpty ? "実行履歴はありません" : "条件に合う実行はありません", systemImage: "clock")
          } else if !loaded { ProgressView() }
        }
      }
      .frame(minWidth: 220, idealWidth: 260)
      VStack(spacing: 0) {
        if runId == nil {
          ContentUnavailableView("実行を選んでください", systemImage: "list.bullet.rectangle")
        } else {
          HStack {
            Picker("呼び出し", selection: $invocation) {
              if invocations.isEmpty { Text("呼び出しなし").tag(UInt64?.none) }
              ForEach(invocations, id: \.invocation) { i in
                Text("#\(i.invocation ?? 0) · \(i.state.label)" + (i.durationMs.map { " · \(formatDuration(Double($0) / 1000))" } ?? ""))
                  .tag(i.invocation)
              }
            }
            .frame(maxWidth: 320)
            Spacer()
            Picker("", selection: $detail) { ForEach(Detail.allCases) { Text($0.title).tag($0) } }
              .pickerStyle(.segmented).labelsHidden().frame(width: 220)
          }
          .padding(6)
          Divider()
          switch detail {
          case .io: ioView
          case .log:
            if let runId { StageLogView(session: session, runId: runId, card: path(cardKey), invocation: invocation) }
          case .info: infoView
          }
        }
        if let error { Text(error).font(.caption).foregroundStyle(.red).padding(6) }
      }
      .frame(minWidth: 380)
    }
    .task(id: runId) { await loadInvocations() }
    .task(id: "\(runId ?? "")#\(invocation ?? 0)") { await loadRecord() }
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
        if let s = r.record?.summary {
          LabeledContent("呼び出し", value: "#\(s.invocation ?? 0)")
          LabeledContent("段階", value: String(describing: s.phase))
          LabeledContent("開始", value: formatMillis(s.startedAt))
          LabeledContent("終了", value: formatMillis(s.endedAt))
          if let d = s.durationMs { LabeledContent("所要時間", value: formatDuration(Double(d) / 1000)) }
          if !s.waitingInputs.isEmpty {
            LabeledContent("待っていた入力", value: s.waitingInputs.compactMap { $0?.port }.joined(separator: ", "))
          }
        }
        if let f = r.status.failure {
          LabeledContent("失敗の種類", value: String(describing: f.kind))
          Text(f.diagnostic ?? "").textSelection(.enabled).foregroundStyle(.red)
        }
        LabeledContent("集計", value: "呼び出し \(r.aggregate.calls ?? 0) · 成功 \(r.aggregate.succeeded ?? 0) · 失敗 \(r.aggregate.failed ?? 0)")
        if let srcs = r.record?.inputSources, !srcs.isEmpty {
          Section("入力元") {
            ForEach(Array(srcs.compactMap { $0 }.enumerated()), id: \.offset) { _, s in
              LabeledContent(s.port, value: "\(s.card.path.joined(separator: "/")) #\(s.invocation ?? 0)")
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
      runs = try await rpc.history(card: path(cardKey), limit: 100).runs.compactMap { $0 }
      if runId == nil || !runs.contains(where: { $0.status.runId == runId }) { runId = runs.first?.status.runId }
      error = nil
    } catch { self.error = String(localized: "履歴を読み込めません: \(error)") }
    loaded = true
  }

  private func loadInvocations() async {
    // Another run: its own latest call is shown, never the previous run's number.
    invocation = nil
    record = nil
    guard let rpc = session.rpc, let runId else { invocations = []; return }
    invocations = (try? await rpc.invocations(runId: runId, card: path(cardKey)).records.compactMap { $0 }) ?? []
    invocation = invocations.last?.invocation
  }

  private func loadRecord() async {
    guard let rpc = session.rpc, let runId else { record = nil; return }
    record = try? await rpc.record(runId: runId, card: path(cardKey), invocation: invocation, followLatest: invocation == nil)
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
    .task(id: session.lastFinished) { await loadRuns() }
  }

  private var content: some View {
    VStack(spacing: 0) {
      HStack {
        Picker("実行", selection: $runId) {
          if runs.isEmpty { Text("成果物のある実行はありません").tag(String?.none) }
          ForEach(runs, id: \.status.runId) { r in
            Text("\(formatMillis(r.status.acceptedAt)) · \(r.status.state.label)").tag(Optional(r.status.runId))
          }
        }
        .frame(maxWidth: 360)
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
      if let img = NSImage(data: data), ["png", "jpg", "jpeg", "gif", "tiff", "pdf", "svg"].contains((member as NSString).pathExtension.lowercased()) {
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
    .task { await session.refreshVisibleDisplays() }
  }
}
