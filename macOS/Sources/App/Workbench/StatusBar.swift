import AppKit
import SwiftUI
import TsugumiRPC

/// Floating bar at the canvas's bottom right: run state and save/sync state
/// shown separately, the problems, and the toggles of the log/timeline panels.
struct StatusBar: View {
  @Bindable var workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab
  @State private var showSync = false
  @State private var showIssues = false
  @State private var confirmDiscard = false
  @Environment(\.openWindow) private var openWindow

  var body: some View {
    // All four layouts keep a single line; the last keeps its intrinsic width
    // even when the canvas offers less room than the icons need.
    ViewThatFits(in: .horizontal) {
      bar(compact: false)
      bar(compact: true)
      HStack(spacing: 8) {
        runState(short: true).fixedSize()
        syncState(short: true).fixedSize()
        Spacer(minLength: 4)
        panelPicker.fixedSize()
      }
      .fixedSize()
      HStack(spacing: 8) {
        runState(short: true, icons: true)
        syncState(short: true, icons: true)
        panelPicker
      }
      .fixedSize()
    }
    .sheet(item: Binding(get: { session.reloadApproval }, set: { session.reloadApproval = $0 })) { request in
      PermissionPromptView(prompt: ChatController.PermissionPrompt(raw: request.permissions, answer: { _ in }),
                           answer: { allow, _, selected in Task { await session.answerReloadApproval(allow, selected: selected) } }, reload: true)
    }
    .onChange(of: session.mergeEdit.conflictRev) { if session.mergeEdit.conflictRev != nil { showSync = true } }
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
    HStack(spacing: compact ? 8 : 14) {
      runState(short: compact).fixedSize()
      Divider().frame(height: 14)
      syncState(short: compact).fixedSize()
      if !compact {
        revision.fixedSize()
        Divider().frame(height: 14)
        counts.fixedSize()
      }
      Spacer(minLength: 8)
      if !compact, let n = session.notice {
        Label(n, systemImage: "info.circle").lineLimit(1).truncationMode(.tail).foregroundStyle(.secondary).help(n)
          .frame(maxWidth: 260)
        Button { session.notice = nil } label: { Image(systemName: "xmark") }.buttonStyle(.borderless)
      }
      panelPicker.fixedSize()
    }
    .fixedSize(horizontal: true, vertical: false)
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

  private func runState(short: Bool, icons: Bool = false) -> some View {
    let s = session.runStatus?.state ?? .idle
    return HStack(spacing: 4) {
      Image(systemName: s.symbol).foregroundStyle(s.color)
      if !icons { Text(short ? s.label : String(localized: "実行: \(s.label)")) }
      if !short, let f = session.runStatus?.failure, s == .failed {
        Text("· \(f.diagnostic ?? String(describing: f.kind))").lineLimit(1).foregroundStyle(.secondary).frame(maxWidth: 260)
      }
    }
    .lineLimit(1)
    .help(s.label)
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(s.label)
  }

  private func syncState(short: Bool, icons: Bool = false) -> some View {
    Button { showSync.toggle() } label: {
      HStack(spacing: 4) {
        switch session.syncState {
        case .saved:
          Image(systemName: "checkmark.icloud").foregroundStyle(.green)
          if !icons { Text("保存済み") }
        case .waitingForRun:
          Image(systemName: "clock.arrow.circlepath").foregroundStyle(.orange)
          if !icons { Text(short ? String(localized: "実行後に保存") : String(localized: "実行が終わったら保存します")) }
        case .saving:
          ProgressView().controlSize(.mini)
          if !icons { Text("保存中…") }
        case .failed:
          Image(systemName: "exclamationmark.icloud.fill").foregroundStyle(.red)
          if !icons { Text("同期できません") }
        case .offline:
          Image(systemName: "icloud.slash").foregroundStyle(.secondary)
          if !icons { Text("未接続") }
        }
      }
    }
    .lineLimit(1)
    .buttonStyle(StatusButtonStyle())
    .help(syncLabel + " · " + String(localized: "保存・同期の詳細"))
    .accessibilityLabel(syncLabel)
    .guideTarget("save")
    .popover(isPresented: $showSync, arrowEdge: .top) { syncDetail }
  }

  private var syncLabel: String {
    switch session.syncState {
    case .saved: return String(localized: "保存済み")
    case .waitingForRun: return String(localized: "実行が終わったら保存します")
    case .saving: return String(localized: "保存中…")
    case .failed: return String(localized: "同期できません")
    case .offline: return String(localized: "未接続")
    }
  }

  /// The saved revision; the project's versions (git) open from it.
  private var revision: some View {
    Button(action: openVersions) {
      HStack(spacing: 4) {
        Image(systemName: "point.3.connected.trianglepath.dotted").foregroundStyle(.secondary)
        Text(verbatim: "rev \(session.rev)").monospacedDigit()
      }
    }
    .buttonStyle(StatusButtonStyle())
    .help(String(localized: "版の履歴を開く（確定 revision \(session.rev)、⌥⌘Y）"))
    .accessibilityLabel(String(localized: "版の履歴"))
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
      RevisionEditNotice(edit: session.mergeEdit, text: { session.unsavedText },
                         overwrite: { session.resolveMergeConflict() }, reload: { session.discardDraft() })
      Text("変更は操作するたびに保存されます。チャットや別のウインドウでの変更もすぐに反映されます。")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      HStack {
        if session.saveError != nil {
          Button("もう一度保存") { Task { await session.save() } }.disabled(!session.connected)
          Button("変更を破棄…", role: .destructive) { confirmDiscard = true }
        }
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

  private var counts: some View {
    let errors = session.diagnostics.filter(\.error).count
    let warnings = session.diagnostics.count - errors
    return HStack(spacing: 10) {
      Button { showIssues.toggle() } label: {
        HStack(spacing: 6) {
          HStack(spacing: 2) { Image(systemName: "xmark.octagon"); Text("\(errors)") }
            .foregroundStyle(errors > 0 ? .red : .secondary)
          HStack(spacing: 2) { Image(systemName: "exclamationmark.triangle"); Text("\(warnings)") }
            .foregroundStyle(warnings > 0 ? .yellow : .secondary)
        }
      }
      .buttonStyle(StatusButtonStyle())
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
