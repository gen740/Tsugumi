import SwiftUI
import TsugumiRPC

/// The Card window's Log: the latest invocation of this Card (the running one
/// while it runs), or the one opened from the execution history.
struct CardLogView: View {
  let session: ProjectSession
  @Bindable var model: CardEditorModel
  /// The newest run in this Card's history, when the Card is not running.
  @State private var latest: Tsugumi_RPC_RunSummaryT?
  @State private var loaded = false
  @State private var error: String?

  private var cardKey: CardKey { model.cardKey }
  /// The Card takes part in the run going on now.
  private var inActiveRun: Bool { session.runActive && session.cardStates[cardKey] != nil }

  private var shown: CardLogSelection? {
    if let s = model.logSelection { return s }
    if inActiveRun, let run = session.currentRunId { return CardLogSelection(runId: run, invocation: nil) }
    return latest?.status.runId.map { CardLogSelection(runId: $0, invocation: nil) }
  }

  var body: some View {
    VStack(spacing: 0) {
      HStack(spacing: 8) {
        Image(systemName: model.logSelection == nil ? (inActiveRun ? "play.circle" : "clock") : "pin")
          .foregroundStyle(.secondary)
        Text(caption).lineLimit(1).truncationMode(.middle)
        Spacer()
        if model.logSelection != nil {
          Button("最新の呼び出しに戻る") { model.logSelection = nil }
        }
        Button("実行履歴") { model.category = .executions }
      }
      .font(.callout).padding(.horizontal, 8).padding(.vertical, 6)
      Divider()
      if let shown {
        StageLogView(session: session, runId: shown.runId, card: path(cardKey), invocation: shown.invocation)
      } else if !loaded {
        ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
      } else if let error {
        ContentUnavailableView("履歴を読み込めませんでした", systemImage: "exclamationmark.triangle", description: Text(error))
      } else {
        ContentUnavailableView("まだ実行していません", systemImage: "text.alignleft",
                               description: Text("この Card が実行されると、ここに段階ごとのログが出ます。実行中は出力をそのまま追います。"))
      }
    }
    .task(id: ExecutionHistoryRefresh(session: session, card: cardKey)) { await loadLatest() }
  }

  private var caption: String {
    if let s = model.logSelection {
      let inv = s.invocation.map { String(localized: "\($0) 回目") } ?? String(localized: "最後の呼び出し")
      return String(localized: "実行履歴から開いた呼び出し: \(runTime(s.runId)) · \(inv)")
    }
    if inActiveRun { return String(localized: "進行中の実行（最新の呼び出しを追います）") }
    if let latest { return String(localized: "最後の実行: \(formatMillis(latest.status.acceptedAt))") }
    return ""
  }

  private func runTime(_ id: String) -> String {
    if id == latest?.status.runId, let latest { return formatMillis(latest.status.acceptedAt) }
    if id == session.currentRunId, let s = session.runStatus { return formatMillis(s.acceptedAt) }
    return id
  }

  private func loadLatest() async {
    defer { if !Task.isCancelled { loaded = true } }
    guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
    do {
      let page = try await rpc.history(card: path(cardKey), limit: 1)
      guard !Task.isCancelled else { return }
      latest = page.runs.first ?? nil
      error = nil
    } catch {
      guard !Task.isCancelled else { return }
      self.error = "\(error)"
    }
  }
}

/// What a Card depends on is part of its definition: default.nix chooses the
/// SDK, the nixpkgs packages and the other Cards it takes outputs from. The
/// packages are shown as the last invocation recorded them; the current
/// definition is not evaluated for this.
struct DependenciesCategory: View {
  @Bindable var model: CardEditorModel
  private struct Recorded {
    let runId: String
    let acceptedAt: Int64?
    let invocation: UInt64
    let environmentId: String
  }
  @State private var latest: Recorded?
  @State private var loaded = false

  private var session: ProjectSession { model.session }
  private var hasDefinition: Bool { model.files.contains { $0.hasSuffix("/default.nix") } }

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 14) {
        Text("この Card が使う SDK、nixpkgs の package、上流 Card の Nix exports は、Nix 定義（default.nix）で決まります。変えるときは default.nix を編集します。")
          .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        HStack {
          if hasDefinition {
            Button("default.nix を開く") { model.openDefinition() }
          } else if model.kind == .code {
            Text("default.nix がありません").foregroundStyle(.secondary)
          }
        }

        Text("最後に記録された環境").font(.headline)
        if !loaded {
          ProgressView()
        } else if let latest {
          Text("実行 \(formatMillis(latest.acceptedAt)) の \(latest.invocation) 回目で使われた環境です。default.nix を変えた後は、次に実行したときに変わります。")
            .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
          RecordedEnvironmentView(session: session, runId: latest.runId, environmentId: latest.environmentId)
            .id(latest.runId + ":" + latest.environmentId)
        } else {
          Text("まだ記録された環境はありません。実行すると、使った環境がここに出ます。").foregroundStyle(.secondary)
        }
      }
      .padding(12)
      .frame(maxWidth: .infinity, alignment: .leading)
    }
    .task(id: ExecutionHistoryRefresh(session: session, card: model.cardKey)) { await load() }
  }

  /// The newest run that recorded an environment for this Card's last invocation.
  private func load() async {
    defer { if !Task.isCancelled { loaded = true } }
    guard let rpc = session.rpc else { return }
    guard let runs = try? await rpc.history(card: path(model.cardKey), limit: 10).runs.compactMap({ $0 }) else { return }
    for run in runs {
      guard let runId = run.status.runId,
            let record = try? await rpc.record(runId: runId, card: path(model.cardKey), followLatest: true) else { continue }
      guard !Task.isCancelled else { return }
      if let id = record.record?.summary.environmentId, !id.isEmpty {
        latest = Recorded(runId: runId, acceptedAt: run.status.acceptedAt,
                          invocation: record.selectedInvocation ?? 0, environmentId: id)
        return
      }
    }
    latest = nil
  }
}
