import AppKit
import SwiftUI
import TsugumiRPC

/// Floating bar at the canvas's bottom right: run state and save/sync state
/// shown separately, counts, and the toggles of the log/timeline panels.
struct StatusBar: View {
  @Bindable var workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab
  @State private var showSync = false
  @State private var showIssues = false
  @State private var confirmDiscard = false

  var body: some View {
    // Full, compact and minimal layouts: the bar never overlaps itself, and
    // the run state and the save state stay visible at any width.
    ViewThatFits(in: .horizontal) {
      bar(compact: false)
      bar(compact: true)
      HStack(spacing: 8) {
        runState(short: true)
        syncState(short: true)
        Spacer(minLength: 4)
        panelPicker(icons: true)
      }
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
    HStack(spacing: compact ? 8 : 14) {
      runState(short: compact).fixedSize()
      Divider().frame(height: 14)
      syncState(short: compact).fixedSize()
      if !compact {
        Divider().frame(height: 14)
        counts.fixedSize()
      }
      Spacer(minLength: 8)
      if !compact, let n = session.notice {
        Label(n, systemImage: "info.circle").lineLimit(1).truncationMode(.tail).foregroundStyle(.secondary).help(n)
          .frame(maxWidth: 260)
        Button { session.notice = nil } label: { Image(systemName: "xmark") }.buttonStyle(.borderless)
      }
      panelPicker(icons: compact).fixedSize()
    }
    .fixedSize(horizontal: true, vertical: false)
  }

  /// Log, timeline and terminal icons: each opens its panel, a second click closes
  /// it, and only the open one is highlighted.
  private func panelPicker(icons: Bool) -> some View {
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
  }

  private func runState(short: Bool) -> some View {
    let s = session.runStatus?.state ?? .idle
    return HStack(spacing: 4) {
      Image(systemName: s.symbol).foregroundStyle(s.color)
      Text(short ? s.label : String(localized: "実行: \(s.label)"))
      if !short, let f = session.runStatus?.failure, s == .failed {
        Text("· \(f.diagnostic ?? String(describing: f.kind))").lineLimit(1).foregroundStyle(.secondary).frame(maxWidth: 260)
      }
    }
    .help(s.label)
    .accessibilityElement(children: .combine)
  }

  private func syncState(short: Bool) -> some View {
    Button { showSync.toggle() } label: {
      HStack(spacing: 4) {
        switch session.syncState {
        case .saved:
          Image(systemName: "checkmark.icloud").foregroundStyle(.green)
          Text(short ? String(localized: "保存済み") : String(localized: "保存済み · rev \(session.rev)"))
        case .waitingForRun:
          Image(systemName: "clock.arrow.circlepath").foregroundStyle(.orange)
          Text(short ? String(localized: "実行後に保存") : String(localized: "実行が終わったら保存します"))
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
    .buttonStyle(StatusButtonStyle())
    .help("保存・同期の詳細")
    .popover(isPresented: $showSync, arrowEdge: .top) { syncDetail }
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
        if session.saveError != nil {
          Button("もう一度保存") { Task { await session.save() } }.disabled(!session.connected)
          Button("変更を破棄…", role: .destructive) { confirmDiscard = true }
        }
        Button("外部の変更を再読込") { Task { await session.reloadExternal() } }
          .disabled(!session.connected)
          .help("Tsugumi の外で編集されたファイルを取り込みます")
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
