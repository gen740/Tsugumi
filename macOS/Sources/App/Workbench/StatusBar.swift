import AppKit
import SwiftUI
import TsugumiRPC

/// Bottom bar: run state and save/sync state shown separately, counts, and
/// the toggle for the log/timeline panel.
struct StatusBar: View {
  @Bindable var workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab
  @State private var showSync = false
  @State private var showIssues = false
  @State private var confirmDiscard = false

  var body: some View {
    HStack(spacing: 14) {
      runState.fixedSize()
      Divider().frame(height: 14)
      syncState.fixedSize()
      if session.remoteChangePending && session.dirty {
        Label("他の場所で更新されました", systemImage: "arrow.triangle.2.circlepath")
          .foregroundStyle(.orange)
          .help("変更は revision \(session.draftBaseRev) を基準にしています。保存すると競合として検出されます。")
      }
      Divider().frame(height: 14)
      counts.fixedSize()
      Spacer(minLength: 8)
      if let n = session.notice {
        Label(n, systemImage: "info.circle").lineLimit(1).truncationMode(.tail).foregroundStyle(.secondary).help(n)
          .layoutPriority(-1)
        Button { session.notice = nil } label: { Image(systemName: "xmark") }.buttonStyle(.borderless)
      }
      Picker("", selection: Binding(get: { workspace.showBottom ? workspace.bottomPanel : nil },
                                    set: { v in
                                      if let v { workspace.bottomPanel = v; workspace.showBottom = true } else { workspace.showBottom = false }
                                    })) {
        Text("閉じる").tag(BottomPanel?.none)
        ForEach(BottomPanel.allCases) { Text($0.title).tag(Optional($0)) }
      }
      .pickerStyle(.segmented).labelsHidden().fixedSize()
      .help("全体ログと時間軸上の実行状況")
    }
    .font(.callout)
    .padding(.horizontal, 10).padding(.vertical, 5)
    .background(.bar)
    .overlay(alignment: .top) { Divider() }
    .confirmationDialog("未保存の変更を破棄して最新の内容を読み込みますか？", isPresented: $confirmDiscard) {
      Button("変更を破棄して読み込む", role: .destructive) {
        session.discardDraft()
        Task { await session.reloadExternal() }
      }
      Button("キャンセル", role: .cancel) {}
    } message: { Text("破棄した変更は取り消せません。必要なら先に「変更をコピー」してください。") }
  }

  private var runState: some View {
    let s = session.runStatus?.state ?? .idle
    return HStack(spacing: 4) {
      Image(systemName: s.symbol).foregroundStyle(s.color)
      Text("実行: \(s.label)")
      if let f = session.runStatus?.failure, s == .failed {
        Text("· \(f.diagnostic ?? String(describing: f.kind))").lineLimit(1).foregroundStyle(.secondary)
      }
    }
    .accessibilityElement(children: .combine)
  }

  private var syncState: some View {
    Button { showSync.toggle() } label: {
      HStack(spacing: 4) {
        switch session.syncState {
        case .saved:
          Image(systemName: "checkmark.icloud").foregroundStyle(.green)
          Text("保存済み · rev \(session.rev)")
        case .unsaved:
          Image(systemName: "pencil.circle.fill").foregroundStyle(.orange)
          Text("未保存の変更")
        case .saving:
          ProgressView().controlSize(.mini)
          Text("保存中…")
        case .failed:
          Image(systemName: "exclamationmark.icloud.fill").foregroundStyle(.red)
          Text("同期できません")
        case .offline:
          Image(systemName: "icloud.slash").foregroundStyle(.secondary)
          Text("未接続")
        }
      }
    }
    .buttonStyle(.borderless)
    .popover(isPresented: $showSync, arrowEdge: .top) { syncDetail }
  }

  private var syncDetail: some View {
    VStack(alignment: .leading, spacing: 10) {
      Text("保存・同期").font(.headline)
      LabeledContent("確定 revision", value: "\(session.rev)")
      LabeledContent("実行 revision", value: "\(session.executionRev)")
      if session.dirty { LabeledContent("変更の基準", value: "rev \(session.draftBaseRev)") }
      if case .failed(let m) = session.syncState {
        Text(m).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
      }
      if session.dirty {
        Text("未保存の変更は保持しています。").font(.callout).foregroundStyle(.secondary)
      }
      HStack {
        Button("保存") { Task { await session.save() } }.disabled(!session.dirty || !session.connected)
        Button("変更をコピー") {
          NSPasteboard.general.clearContents()
          NSPasteboard.general.setString(session.draftSummary(), forType: .string)
        }
        .disabled(!session.dirty)
        Button("外部の変更を再読込") { Task { await session.reloadExternal() } }
          .disabled(!session.connected)
          .help("Tsugumi の外で編集されたファイルを取り込みます（未保存の変更は保持）")
        Button("最新を読み込む…", role: .destructive) { confirmDiscard = true }
          .disabled(!session.dirty)
          .help("未保存の変更を破棄します（確認があります）")
      }
    }
    .padding(14)
    .frame(width: 440)
  }

  private var counts: some View {
    let cards = session.cards(in: tab.scope).count
    let conns = session.connections(in: tab.scope).count
    let errors = session.diagnostics.filter(\.error).count
    let warnings = session.diagnostics.count - errors
    return HStack(spacing: 10) {
      Text("Card \(cards)").help("この階層の Card")
      Text("接続 \(conns)").help("この階層の接続")
      Button { showIssues.toggle() } label: {
        HStack(spacing: 6) {
          HStack(spacing: 2) { Image(systemName: "xmark.octagon"); Text("\(errors)") }
            .foregroundStyle(errors > 0 ? .red : .secondary)
          HStack(spacing: 2) { Image(systemName: "exclamationmark.triangle"); Text("\(warnings)") }
            .foregroundStyle(warnings > 0 ? .yellow : .secondary)
        }
      }
      .buttonStyle(.borderless)
      .help("問題一覧")
      .popover(isPresented: $showIssues, arrowEdge: .top) {
        IssueList(workspace: workspace, session: session, tab: tab).frame(width: 460, height: 320)
      }
    }
  }
}

/// Diagnostics and Card failures; a row jumps to its Card or its log.
struct IssueList: View {
  let workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab

  struct Issue: Identifiable {
    let id = UUID()
    let error: Bool
    let title: String
    let detail: String
    let card: [String]?
    let isRunFailure: Bool
  }

  var issues: [Issue] {
    var out: [Issue] = []
    for (k, s) in session.cardStates.sorted(by: { $0.key < $1.key }) where s.state == .failed || s.state == .blocked {
      out.append(Issue(error: s.state == .failed, title: "\(session.displayName(k)): \(s.state.label)",
                       detail: s.failure ?? "", card: path(k), isRunFailure: true))
    }
    for d in session.diagnostics {
      out.append(Issue(error: d.error, title: d.message,
                       detail: [d.card.map { $0.joined(separator: "/") }, d.path, d.code].compactMap { $0 }.joined(separator: " · "),
                       card: d.card, isRunFailure: false))
    }
    return out
  }

  var body: some View {
    let list = issues
    if list.isEmpty {
      ContentUnavailableView("問題はありません", systemImage: "checkmark.seal")
    } else {
      List(list) { i in
        HStack(alignment: .top) {
          Image(systemName: i.error ? "xmark.circle.fill" : "exclamationmark.triangle.fill")
            .foregroundStyle(i.error ? .red : .yellow)
          VStack(alignment: .leading) {
            Text(i.title)
            if !i.detail.isEmpty { Text(i.detail).font(.caption).foregroundStyle(.secondary).lineLimit(3) }
          }
          Spacer()
          if let c = i.card {
            Button("表示") { reveal(c) }.buttonStyle(.borderless)
            if i.isRunFailure { Button("ログ") { workspace.showLog(for: key(c)) }.buttonStyle(.borderless) }
          }
        }
      }
    }
  }

  private func reveal(_ card: [String]) {
    let scope = Array(card.dropLast())
    tab.enter(scope)
    tab.selection = [key(card)]
  }
}
