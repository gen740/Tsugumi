import AppKit
import SwiftUI
import TsugumiRPC

/// Left column: workflow hierarchy, new Cards (each with a definition folder
/// of its own) and the Cards shown in the current scope.
struct WorkbenchSidebar: View {
  @Environment(\.openWindow) private var openWindow
  @Bindable var workspace: Workspace
  let session: ProjectSession
  @Bindable var tab: WorkflowTab
  @State private var defSearch = ""
  /// Expanded tree nodes; kept across searches and reloads.
  @State private var expanded: Set<String> = ["origin:"]
  @State private var importing = false

  var body: some View {
    VStack(spacing: 0) {
      List(selection: Binding(get: { tab.selection }, set: { tab.selection = $0 })) {
        Section("Workflow 階層") {
          ScopeRow(workspace: workspace, session: session, tab: tab, scope: [], depth: 0)
        }
        Section {
          definitions
        } header: {
          HStack {
            Text("Card を追加")
            Spacer()
            Menu {
              Button("他のプロジェクトからコピー…") { importing = true }
              Button("Card の定義を一覧…") { openWindow(id: "definitions", value: ProjectRef(path: session.identity.url.path)) }
            } label: { Image(systemName: "ellipsis.circle") }
              .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
              .disabled(!session.isEditable)
              .help("他のプロジェクトから Card をコピー")
          }
        }
        Section("表示中の Card") {
          VisibleCards(session: session, tab: tab)
        }
      }
      .listStyle(.sidebar)
      .safeAreaInset(edge: .bottom, spacing: 0) {
        // Workflow settings (this project), apart from the app settings (⌘,).
        HStack {
          Button {
            openWindow(id: "workflow-settings", value: ProjectRef(path: session.identity.url.path))
          } label: { Image(systemName: "gearshape").font(.system(size: 13)).frame(width: 26, height: 22) }
            .buttonStyle(.borderless)
            .help("Workflow 設定")
            .accessibilityLabel("Workflow 設定")
          Spacer()
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
      }
    }
    .sheet(isPresented: $importing) { ImportDefinitionsSheet(session: session) }
  }

  @ViewBuilder private var definitions: some View {
    TextField("検索", text: $defSearch).textFieldStyle(.roundedBorder)
    ForEach(DefinitionTemplate.allCases.filter { defSearch.isEmpty || $0.label.localizedCaseInsensitiveContains(defSearch) }) { t in
      HStack(spacing: 6) {
        Image(systemName: t.symbol).foregroundStyle(.secondary).frame(width: 16)
        Text(t.label).lineLimit(1)
        Spacer()
      }
      .contentShape(Rectangle())
      .draggable(DefinitionDrag.templatePrefix + t.rawValue)
      .onTapGesture(count: 2) { create(t) }
      .help("ダブルクリックまたは Canvas へドラッグして追加。Card ごとに定義のフォルダを作ります。")
      .contextMenu { Button("Canvas に追加") { create(t) }.disabled(!session.isEditable) }
      .selectionDisabled()
    }
    // Definition folders without a Card (copied from another project).
    let unplaced = session.unplacedDefinitions.filter { defSearch.isEmpty || $0.localizedCaseInsensitiveContains(defSearch) }
    if !unplaced.isEmpty {
      DisclosureGroup(isExpanded: Binding(get: { expanded.contains("unplaced") || !defSearch.isEmpty },
                                          set: { if $0 { expanded.insert("unplaced") } else { expanded.remove("unplaced") } })) {
        ForEach(unplaced, id: \.self) { id in
          HStack(spacing: 6) {
            Image(systemName: definitionKind(session.definitions[id]).symbol).foregroundStyle(.secondary).frame(width: 16)
            Text(id).lineLimit(1)
            Spacer()
          }
          .contentShape(Rectangle())
          .draggable(DefinitionDrag.prefix + id)
          .onTapGesture(count: 2) { place(id) }
          .help("ダブルクリックまたは Canvas へドラッグして配置")
          .contextMenu {
            Button("Canvas に配置") { place(id) }.disabled(!session.isEditable)
            Button("削除", role: .destructive) { session.edit { $0.removeDefinition(id) } }.disabled(!session.isEditable)
          }
          .selectionDisabled()
        }
      } label: {
        Label("配置されていない定義", systemImage: "tray").font(.callout).foregroundStyle(.secondary)
      }
      .selectionDisabled()
    }
  }

  private var dropPoint: CGPoint {
    let cam = session.cameras[key(tab.scope)] ?? (40, 40, 1)
    return CGPoint(x: (240 - cam.x) / cam.zoom, y: (160 - cam.y) / cam.zoom)
  }

  private func place(_ id: String) {
    guard session.isEditable, tab.scope.isEmpty else { return }
    tab.selection = [session.placeDefinition(id, at: dropPoint)]
  }

  private func create(_ t: DefinitionTemplate) {
    guard session.isEditable else { return }
    if !tab.scope.isEmpty { tab.enter([]) }
    let k = session.addCard(template: t, in: [], at: dropPoint)
    tab.selection = [k]
    if t == .draft {
      openWindow(id: "draft", value: CardRef(path: session.identity.url.path, card: k))
    }
  }
}

func definitionKind(_ def: JSONValue?) -> CardKind {
  if let t = def?["template"]?.string, let k = CardKind(rawValue: t) { return k }
  return def?["language"] == nil ? .draft : .code
}

/// Dragging onto the canvas: a template makes a new Card, a definition
/// folder without a Card is placed (carried as prefixed strings).
enum DefinitionDrag {
  static let prefix = "tsugumi-definition:"
  static let templatePrefix = "tsugumi-template:"
  static func id(_ s: String) -> String? { s.hasPrefix(prefix) ? String(s.dropFirst(prefix.count)) : nil }
  static func template(_ s: String) -> DefinitionTemplate? {
    s.hasPrefix(templatePrefix) ? DefinitionTemplate(rawValue: String(s.dropFirst(templatePrefix.count))) : nil
  }
}

/// One scope in the hierarchy, with its child workflows.
struct ScopeRow: View {
  @Environment(\.openWindow) private var openWindow
  let workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab
  let scope: [String]
  let depth: Int
  @State private var expanded = true

  var body: some View {
    let children = session.childScopes(of: scope)
    let current = tab.scope == scope
    let row = HStack {
      Image(systemName: scope.isEmpty ? "square.stack.3d.up.fill" : "square.stack.3d.up")
        .foregroundStyle(current ? Color.accentColor : .secondary)
      Text(session.scopeName(scope)).fontWeight(current ? .semibold : .regular).lineLimit(1)
      Spacer()
      if current { Text("表示中").font(.caption2).foregroundStyle(.secondary) }
    }
    .contentShape(Rectangle())
    .onTapGesture { tab.enter(scope) }
    .contextMenu {
      Button("このタブで開く") { tab.enter(scope) }
      Button("新しいタブで開く") { workspace.openTab(scope) }
      Button("別ウインドウで開く") { openWindow(id: "workflow", value: WorkflowRef(path: session.identity.url.path, scope: scope)) }
    }
    .selectionDisabled()
    if children.isEmpty {
      row
    } else {
      DisclosureGroup(isExpanded: $expanded) {
        ForEach(children, id: \.self) { c in
          ScopeRow(workspace: workspace, session: session, tab: tab, scope: c, depth: depth + 1)
        }
      } label: { row }
    }
  }
}

/// The Cards of the current scope, under the headings of their visual
/// groups. A row selects its Card and moves the canvas to it; a composite
/// Card has an explicit "開く". Collapsing is done on the Card itself.
struct VisibleCards: View {
  let session: ProjectSession
  let tab: WorkflowTab

  var body: some View {
    let ids = session.cards(in: tab.scope).compactMap { $0["id"]?.string }
    let groups = session.groups(in: tab.scope)
    let grouped = Set(groups.flatMap { ($0["cards"]?.array ?? []).compactMap(\.string) })
    if ids.isEmpty { Text("Card はありません").foregroundStyle(.secondary) }
    ForEach(groups.compactMap { g in g["id"]?.string.map { (g, $0) } }, id: \.1) { g, gid in
      let members = (g["cards"]?.array ?? []).compactMap(\.string).filter(ids.contains)
      if !members.isEmpty {
        Label(g["name"]?.string ?? String(localized: "グループ"), systemImage: "rectangle.3.group")
          .font(.caption.weight(.semibold)).foregroundStyle(.secondary)
          .selectionDisabled()
        ForEach(members, id: \.self) { id in row(id, indent: true) }
      }
    }
    ForEach(ids.filter { !grouped.contains($0) }, id: \.self) { id in row(id, indent: !groups.isEmpty) }
  }

  private func row(_ id: String, indent: Bool) -> some View {
    let k = key(tab.scope + [id])
    return VisibleCardRow(session: session, tab: tab, cardKey: k)
      .padding(.leading, indent ? 10 : 0)
      .tag(k)
  }
}

/// A Card of the current scope: name, kind and run state.
struct VisibleCardRow: View {
  let session: ProjectSession
  let tab: WorkflowTab
  let cardKey: CardKey

  var body: some View {
    let state = session.cardStates[cardKey]
    let errors = session.resolution[cardKey]?.diagnostics.filter(\.error).count ?? 0
    let kind = session.kind(cardKey)
    HStack(spacing: 6) {
      Image(systemName: kind.symbol).foregroundStyle(.secondary).frame(width: 14)
      Text(session.displayName(cardKey)).lineLimit(1)
      Spacer()
      if errors > 0 {
        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow).help("問題 \(errors) 件")
      }
      if let state {
        HStack(spacing: 2) {
          Image(systemName: state.state.symbol).foregroundStyle(state.state.color)
          if state.invocation > 0 { Text("#\(state.invocation)").font(.caption2.monospacedDigit()).foregroundStyle(.secondary) }
        }
        .help("\(state.state.label)（実行 #\(state.invocation)）")
      }
      if kind == .subgraph {
        Button("開く") { tab.enter(path(cardKey)) }
          .buttonStyle(.borderless).font(.caption)
          .help("Card in Card の中を表示（読み取り専用。編集は externals のプロジェクトを単体で開いて行います）")
      }
    }
    .contentShape(Rectangle())
    .onTapGesture {
      tab.selection = [cardKey]
      NotificationCenter.default.post(name: .canvasReveal, object: tab.id, userInfo: ["card": cardKey])
    }
    .accessibilityElement(children: .combine)
    .accessibilityLabel("\(session.displayName(cardKey)), \(kind.label), \(state.map { $0.state.label } ?? String(localized: "未実行"))")
  }
}

/// Copies definitions from another project (through a read-only import grant).
struct ImportDefinitionsSheet: View {
  @Environment(\.dismiss) private var dismiss
  let session: ProjectSession
  @State private var folder: URL?
  @State private var available: [(id: String, name: String)] = []
  @State private var chosen: Set<String> = []
  @State private var working = false
  @State private var message: String?

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("他のプロジェクトから定義をコピー").font(.title2.bold())
      HStack {
        Text(folder?.path ?? "コピー元のプロジェクトを選んでください").foregroundStyle(folder == nil ? .secondary : .primary)
          .lineLimit(1).truncationMode(.middle)
        Spacer()
        Button("選択…") { choose() }
      }
      List(available, id: \.id) { d in
        Toggle(isOn: Binding(get: { chosen.contains(d.id) }, set: { if $0 { chosen.insert(d.id) } else { chosen.remove(d.id) } })) {
          VStack(alignment: .leading) {
            Text(d.name)
            Text(d.id).font(.caption).foregroundStyle(.secondary)
          }
        }
      }
      .frame(minHeight: 200)
      Text("定義とソースをこのプロジェクトにコピーします（以後はこのプロジェクトの定義として編集でき、元とは連動しません）。設定はコピー時点の値を、既定値も含めて具体的な値として保存します。ID が重なる場合は新しい ID を割り当てます。Canvas への配置は別に行います。")
        .font(.caption).foregroundStyle(.secondary)
      if session.dirty { Label("コピーの前に未保存の変更を保存してください。", systemImage: "exclamationmark.triangle").foregroundStyle(.orange) }
      if let message { Text(message).font(.callout).textSelection(.enabled) }
      HStack {
        Spacer()
        Button("閉じる") { dismiss() }
        Button("コピー") { run() }
          .disabled(chosen.isEmpty || working || session.dirty || !session.isEditable)
          .keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 520, height: 480)
  }

  private func choose() {
    let p = NSOpenPanel()
    p.canChooseDirectories = true
    p.canChooseFiles = false
    p.message = "tsugumi.json を含むフォルダを選んでください"
    guard p.runModal() == .OK, let url = p.url else { return }
    folder = url
    available = definitionIds(inProject: url)
    chosen = Set(available.map(\.id))
  }

  private func run() {
    guard let folder else { return }
    working = true
    Task {
      do {
        let map = try await session.importDefinitions(from: folder, ids: chosen.sorted())
        let renamed = map.filter { $0.key != $0.value }
        // Remember where each definition came from (shown in the definition tree).
        let imported = chosen.map { map[$0] ?? $0 }
        for _ in 0..<40 where !imported.allSatisfy({ session.definitions[$0] != nil }) {
          try await Task.sleep(for: .milliseconds(50))
        }
        let origin = folder.lastPathComponent
        session.edit { s in
          for id in imported {
            guard let card = s.definitions[id] else { continue }
            var meta = s.definitionMeta[id] ?? .object([])
            meta["origin"] = .string(origin)
            s.setDefinition(id, card: concreteSettings(card), meta: meta)
          }
        }
        await session.save()
        message = "\(chosen.count) 件をコピーしました。" + (renamed.isEmpty ? "" : " 新しい ID: " + renamed.map { "\($0.key) → \($0.value)" }.joined(separator: ", "))
      } catch {
        message = "コピーできませんでした: \(error)"
      }
      working = false
    }
  }
}

/// A copied definition carries every setting as a value: what it had, and
/// the core's default for what it left out (graph.hh: CardSettings).
func concreteSettings(_ card: JSONValue) -> JSONValue {
  var c = card
  let code = c["language"] != nil && c["template"] == nil
  let defaults: [(String, JSONValue)] = [
    ("maxRuns", .number("1")), ("recordIO", .bool(false)), ("retainArtifact", .bool(false)),
    ("filesystem", .bool(false)), ("sandbox", .bool(true)),
    ("timeoutSeconds", .null),
  ] + (code ? [("buildTimeoutSeconds", .null), ("trigger", .string("allInputs")), ("reuseWorker", .bool(true)),
               ("network", .bool(false))] : [])
  for (k, v) in defaults where c[k] == nil { c[k] = v }
  return c
}
