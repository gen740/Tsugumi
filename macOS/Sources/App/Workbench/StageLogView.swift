import AppKit
import SwiftUI
import TsugumiRPC

/// The log of one invocation of a Card by stage, like a CI job: SDK
/// preparation, Nix evaluation and build / execution / validation and save,
/// each with its state, duration and what it printed, folded unless it failed
/// or was asked for. The raw streams stay available under 「stream 別」.
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
  @State private var workerReused: Bool?
  @State private var shownInvocation: UInt64?
  @State private var logs: [Tsugumi_Data_LogStream: String] = [:]
  @State private var open: Set<String> = []
  @State private var rawStream: Tsugumi_Data_LogStream = .stdout
  @State private var error: String?
  @State private var loaded = false

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
      } else if stages.isEmpty && logs.values.allSatisfy(\.isEmpty) {
        ContentUnavailableView("この実行では、この Card はまだ動いていません", systemImage: "circle.dotted")
      } else {
        ScrollView {
          VStack(alignment: .leading, spacing: 4) {
            ForEach(groups) { g in section(g) }
            DisclosureGroup(isExpanded: binding("raw")) {
              VStack(alignment: .leading, spacing: 4) {
                Picker("", selection: $rawStream) {
                  Text("stdout").tag(Tsugumi_Data_LogStream.stdout)
                  Text("stderr").tag(Tsugumi_Data_LogStream.stderr)
                  Text("build").tag(Tsugumi_Data_LogStream.build)
                  Text("journal").tag(Tsugumi_Data_LogStream.journal)
                }
                .pickerStyle(.segmented).labelsHidden().frame(width: 300)
                logText(logs[rawStream] ?? "", empty: String(localized: "この stream への出力はありません"))
              }
            } label: {
              Label("stream 別（元の出力）", systemImage: "list.bullet.rectangle").font(.callout)
            }
            .padding(.horizontal, 8)
          }
          .padding(.vertical, 6)
        }
      }
    }
    .task(id: "\(runId)|\(invocation ?? 0)") { await load() }
    .onChange(of: session.logRevision) { Task { await load() } }
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
          logText(logs[.build] ?? "", empty: String(localized: "準備・評価・build の出力はありません（キャッシュや再利用のときは空です）"))
        case "execute":
          if let mode = workerMode {
            LabeledContent("実行モード", value: mode == "resident" ? String(localized: "状態を保持（常駐 worker）") : String(localized: "呼び出しごとに初期化"))
          }
          if let workerReused {
            Text(workerReused ? String(localized: "この呼び出しは常駐 worker を再利用しました") : String(localized: "この呼び出しは新しい worker で実行しました"))
              .font(.caption).foregroundStyle(.secondary)
          }
          Text("stdout").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
          logText(logs[.stdout] ?? "", empty: String(localized: "stdout への出力はありません"))
          Text("stderr").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
          logText(logs[.stderr] ?? "", empty: String(localized: "stderr への出力はありません"))
        default:
          logText(journal(for: ["validate", "save"]), empty: String(localized: "記録された出来事はありません"))
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

  private func logText(_ text: String, empty: String) -> some View {
    Text(text.isEmpty ? empty : text)
      .font(.system(size: AppPreferences.shared.editorFontSize - 1, design: text.isEmpty ? .default : .monospaced))
      .foregroundStyle(text.isEmpty ? .secondary : .primary)
      .textSelection(.enabled)
      .frame(maxWidth: .infinity, alignment: .leading)
      .padding(6)
      .background(.background.secondary, in: RoundedRectangle(cornerRadius: 5))
  }

  /// Journal lines (JSON) of the given phases, as "event: message".
  private func journal(for phases: [String]) -> String {
    (logs[.journal] ?? "").split(separator: "\n").compactMap { line -> String? in
      guard let v = try? JSONValue.parse(String(line)), let p = v["phase"]?.string, phases.contains(p) else { return nil }
      return "\(p) · \(v["event"]?.string ?? ""): \(v["message"]?.string ?? "")"
    }.joined(separator: "\n")
  }

  private var workerMode: String? {
    (logs[.journal] ?? "").split(separator: "\n").compactMap { line -> String? in
      guard let v = try? JSONValue.parse(String(line)), v["event"]?.string == "worker_mode",
            let mode = v["message"]?.string, ["resident", "fresh"].contains(mode) else { return nil }
      return mode
    }.last
  }

  private func load() async {
    defer { loaded = true }
    guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
    do {
      var inv = invocation
      if inv == nil, runId == session.currentRunId { inv = session.cardStates[key(card)]?.invocation }
      let rec = try? await rpc.record(runId: runId, card: card, invocation: inv, followLatest: inv == nil)
      inv = inv ?? rec?.selectedInvocation
      shownInvocation = inv
      workerReused = rec?.record?.summary.workerReused
      if runId == session.currentRunId, let s = session.cardStates[key(card)], s.invocation == inv {
        stages = s.stages
        state = s.state
        failure = s.failure
      } else if let summary = rec?.record?.summary {
        stages = TimelineEntry.stages(summary.stages)
        state = summary.state
        failure = summary.failure?.diagnostic
      }
      var out: [Tsugumi_Data_LogStream: String] = [:]
      for stream in [Tsugumi_Data_LogStream.build, .stdout, .stderr, .journal] {
        var text = "", offset: UInt64 = 0
        while true {
          let slice = try await rpc.logRange(runId: runId, card: card, invocation: inv, stream: stream, offset: offset, limit: 256 * 1024)
          text += slice.text
          let next = slice.nextOffset ?? offset
          if next == offset || next >= (slice.sizeBytes ?? 0) { break }
          offset = next
        }
        out[stream] = text
      }
      logs = out
      error = nil
      // A failed stage opens by itself; so does the one asked for.
      if open.isEmpty {
        for g in groups where status(g).2 == String(localized: "失敗") { open.insert(g.id) }
        if let f = focus, let g = groups.first(where: { $0.stages.contains(f) }) { open.insert(g.id) }
      }
    } catch let f as RPCFailure where f.kind == .notFound {
      logs = [:]
      error = nil
    } catch {
      self.error = "\(error)"
    }
  }
}
