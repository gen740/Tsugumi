import AppKit
import SwiftUI
import TsugumiRPC

enum WorkbenchNotice: Equatable {
  case info(String)
  case error(String)

  var message: String {
    switch self { case .info(let text), .error(let text): return text }
  }

  var symbol: String {
    switch self { case .info: return "info.circle"; case .error: return "exclamationmark.triangle.fill" }
  }

  var color: Color {
    switch self { case .info: return .secondary; case .error: return .red }
  }
}

/// Diagnostics, revision and the log, timeline and console toggles.
struct StatusBar: View {
  @Bindable var workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab
  @State private var showSync = false
  @State private var showIssues = false
  @State private var confirmDiscard = false
  @Environment(\.openWindow) private var openWindow

  var body: some View {
    ViewThatFits(in: .horizontal) {
      bar(compact: false)
      bar(compact: true)
    }
    .sheet(item: Binding(get: { session.reloadApproval }, set: { session.reloadApproval = $0 })) { request in
      PermissionPromptView(prompt: ChatController.PermissionPrompt(raw: request.permissions, answer: { _ in }),
                           answer: { allow, selected in Task { await session.answerReloadApproval(allow, selected: selected) } }, reload: true)
    }
    .font(.callout)
    .padding(.horizontal, 12).padding(.vertical, 5)
    .background(.regularMaterial, in: Capsule())
    .fixedSize(horizontal: false, vertical: true)
    .confirmationDialog("保存できない変更を破棄して、保存済みの内容に戻しますか？", isPresented: $confirmDiscard) {
      Button("変更を破棄", role: .destructive) { session.discardDraft() }
      Button("キャンセル", role: .cancel) {}
    } message: { Text("破棄した変更は取り消せません。") }
  }

  private func bar(compact: Bool) -> some View {
    HStack(spacing: 6) {
      if session.restartingEnvironment {
        restarting(compact: compact)
        Divider().frame(height: 14)
      }
      counts(compact: compact)
      Divider().frame(height: 14)
      revision(compact: compact)
      Divider().frame(height: 14)
      panelPicker
    }
    .fixedSize(horizontal: true, vertical: false)
  }

  /// The environment restarts to share attached folders; the project stays shown.
  private func restarting(compact: Bool) -> some View {
    HStack(spacing: 4) {
      ProgressView().controlSize(.mini)
      if !compact { Text("実行環境を再起動しています…").foregroundStyle(.secondary) }
    }
    .help(String(localized: "共有するフォルダが変わるため、実行環境を再起動しています"))
    .accessibilityElement(children: .combine)
    .accessibilityLabel(String(localized: "実行環境を再起動しています…"))
  }

  /// Log, timeline and terminal icons: each opens its panel, a second click closes
  /// it, and only the open one is highlighted.
  private var panelPicker: some View {
    HStack(spacing: 2) {
      panelButton(.log, symbol: "text.alignleft", shortcut: .toggleLog)
      panelButton(.timeline, symbol: "chart.bar.xaxis", shortcut: .toggleTimeline)
      panelButton(.terminal, symbol: "terminal", shortcut: .toggleTerminal)
    }
  }

  private func panelButton(_ p: BottomPanel, symbol: String, shortcut: ShortcutAction) -> some View {
    let open = workspace.showBottom && workspace.bottomPanel == p
    return Button { workspace.togglePanel(p) } label: {
      Image(systemName: symbol)
        .frame(width: 22, height: 16)
        .foregroundStyle(open ? Color.white : Color.primary)
    }
    .buttonStyle(StatusButtonStyle(selected: open))
    .help("\(p.title)（\(AppPreferences.shared.shortcut(shortcut).label)）")
    .accessibilityLabel(p.title)
    .accessibilityAddTraits(open ? .isSelected : [])
    .guideTarget(p == .log ? "logs" : p.rawValue)
  }

  private var syncLabel: String {
    switch session.syncState {
    case .saved: return String(localized: "保存済み")
    case .saving: return String(localized: "保存中…")
    case .failed: return String(localized: "同期できません")
    case .offline: return String(localized: "未接続")
    }
  }

  private func revision(compact: Bool) -> some View {
    Button { showSync.toggle() } label: {
      HStack(spacing: 4) {
        switch session.syncState {
        case .saved:
          Image(systemName: "point.3.connected.trianglepath.dotted").foregroundStyle(.secondary)
        case .saving:
          ProgressView().controlSize(.mini)
        case .failed:
          Image(systemName: "exclamationmark.icloud.fill").foregroundStyle(.red)
        case .offline:
          Image(systemName: "icloud.slash").foregroundStyle(.secondary)
        }
        Text(verbatim: "rev \(session.rev)").monospacedDigit()
        if !compact { Text(syncLabel).foregroundStyle(.secondary) }
      }
    }
    .buttonStyle(StatusButtonStyle())
    .help(syncLabel + " · " + String(localized: "保存・同期の詳細"))
    .accessibilityLabel(String(localized: "確定 revision") + " \(session.rev) · " + syncLabel)
    .guideTarget("save")
    .popover(isPresented: $showSync, arrowEdge: .top) { syncDetail }
  }

  private func openVersions() {
    openWindow(id: "versions", value: ProjectRef(path: session.identity.url.path))
  }

  private var syncDetail: some View {
    VStack(alignment: .leading, spacing: 10) {
      Text("保存・同期").font(.headline)
      LabeledContent("確定 revision", value: "\(session.rev)")
      LabeledContent("実行 revision", value: "\(session.executionRev)")
      if case .failed(let m) = session.syncState {
        Text(m).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
      }
      Text("変更は操作するたびに保存されます。チャットや別のウインドウでの変更もすぐに反映されます。")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      HStack {
        Button("外部の変更を再読込") { Task { await session.reloadExternal() } }
          .disabled(!session.connected || session.reloadingExternal || session.reloadApproval != nil)
          .help("Tsugumi の外で編集されたファイルを取り込みます")
        Button("版の履歴…") {
          showSync = false
          openVersions()
        }
        .help("実行ごとに残った版（git）を枝・タグ・実行結果と一緒に見ます")
      }
    }
    .padding(14)
    .frame(width: 440)
  }

  private func counts(compact: Bool) -> some View {
    let list = IssueList(workspace: workspace, session: session, tab: tab)
    let issues = list.issues
    let errors = issues.filter(\.error).count
    let warnings = issues.count - errors
    return Button { showIssues.toggle() } label: {
      HStack(spacing: 4) {
        if compact || issues.isEmpty {
          Image(systemName: issues.isEmpty ? "checkmark.seal" : "exclamationmark.triangle.fill")
            .foregroundStyle(errors > 0 ? .red : warnings > 0 ? .orange : .secondary)
          if !issues.isEmpty { Text("\(issues.count)").monospacedDigit() }
        } else {
          Label("\(errors)", systemImage: "xmark.octagon")
            .foregroundStyle(errors > 0 ? .red : .secondary)
          Label("\(warnings)", systemImage: "exclamationmark.triangle")
            .foregroundStyle(warnings > 0 ? .orange : .secondary)
        }
      }
    }
    .buttonStyle(StatusButtonStyle())
    .help(String(localized: "問題 \(issues.count) 件（一覧から対象の Card やログへ移動できます）"))
    .accessibilityLabel("問題一覧")
    .popover(isPresented: $showIssues, arrowEdge: .top) { list.frame(width: 460, height: 320) }
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
    if let status = session.runStatus, status.state == .failed, let failure = status.failure {
      let detail = failure.diagnosticText ?? String(describing: failure.kind)
      if !out.contains(where: { $0.detail == detail }) {
        out.insert(Issue(error: true, title: status.state.label, detail: detail, card: nil, isRunFailure: true), at: 0)
      }
    }
    for d in session.diagnostics {
      out.append(Issue(error: d.error, title: d.message,
                       detail: [d.card.map { session.displayPath($0) }, d.path, d.code].compactMap { $0 }.joined(separator: " · "),
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
            Text(i.title).fixedSize(horizontal: false, vertical: true)
            if !i.detail.isEmpty {
              Text(i.detail).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
          }
          Spacer()
          if let c = i.card {
            Button("表示") { reveal(c) }.buttonStyle(.borderless)
            if i.isRunFailure { Button("ログ") { workspace.showLog(for: key(c)) }.buttonStyle(.borderless) }
          }
        }
        .textSelection(.enabled)
      }
    }
  }

  private func reveal(_ card: [String]) {
    let scope = Array(card.dropLast())
    tab.enter(scope)
    tab.selection = [key(card)]
  }
}

/// A status bar control that looks pressable: a light background, stronger
/// on hover and when pressed (plain status texts have none).
struct StatusButtonStyle: ButtonStyle {
  var selected = false
  @State private var hovering = false

  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .padding(.horizontal, 6).padding(.vertical, 2)
      .background(RoundedRectangle(cornerRadius: 5).fill(
        selected ? Color.accentColor :
          Color.primary.opacity(configuration.isPressed ? 0.18 : hovering ? 0.12 : 0.06)))
      .overlay(RoundedRectangle(cornerRadius: 5).strokeBorder(Color.primary.opacity(selected ? 0 : 0.1)))
      .contentShape(Rectangle())
      .onHover { hovering = $0 }
  }
}
