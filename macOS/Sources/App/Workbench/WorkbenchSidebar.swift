import AppKit
import SwiftUI
import TsugumiRPC

/// Left column: workflow hierarchy, Card definitions and the Cards shown in
/// the current scope.
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
            Text("Card 定義")
            Spacer()
            Menu {
              ForEach(DefinitionTemplate.allCases) { t in
                Button(t.label) { create(t) }
              }
              Divider()
              Button("他のプロジェクトからコピー…") { importing = true }
              Button("Card 定義を管理…") { openWindow(id: "definitions", value: ProjectRef(path: session.identity.url.path)) }
            } label: { Image(systemName: "plus") }
              .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
              .disabled(!session.isEditable)
              .help("定義を追加")
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
    TextField("定義を検索", text: $defSearch).textFieldStyle(.roundedBorder)
    DefinitionTree(session: session, search: defSearch, expanded: $expanded,
                   onPlace: { place($0) },
                   onReveal: { k in
                     tab.enter(Array(path(k).dropLast()))
                     tab.selection = [k]
                   },
                   onEdit: { id in openWindow(id: "definitions", value: ProjectRef(path: session.identity.url.path, definition: id)) })
  }

  private func place(_ id: String) {
    guard session.isEditable else { return }
    let cam = session.cameras[key(tab.scope)] ?? (40, 40, 1)
    let k = session.addCard(definition: id, in: tab.scope,
                            at: CGPoint(x: (240 - cam.x) / cam.zoom, y: (160 - cam.y) / cam.zoom))
    tab.selection = [k]
  }

  private func create(_ t: DefinitionTemplate) {
    let id = session.createDefinition(name: t == .draft ? "新しい Draft" : "新しい Card", template: t)
    place(id)
    if t == .draft, let k = tab.selection.first {
      openWindow(id: "draft", value: CardRef(path: session.identity.url.path, card: k))
    }
  }
}

/// Card definitions as a tree of what contains what: the project (or the
/// project a definition was copied from), its composite Cards (Card in
/// Card, nested as on the canvas) and the definitions used inside each.
/// Plain definitions are leaves. Where a definition is used is listed
/// separately (its menu), not as children.
struct DefinitionTree: View {
  let session: ProjectSession
  let search: String
  @Binding var expanded: Set<String>
  var onPlace: (String) -> Void
  var onReveal: (CardKey) -> Void
  var onEdit: (String) -> Void

  private func matches(_ id: String) -> Bool {
    search.isEmpty || id.localizedCaseInsensitiveContains(search) ||
      (session.definitions[id]?["name"]?.string ?? "").localizedCaseInsensitiveContains(search) ||
      (session.definitionMeta[id]?["description"]?.string ?? "").localizedCaseInsensitiveContains(search)
  }

  private func origin(_ id: String) -> String { session.definitionMeta[id]?["origin"]?.string ?? "" }

  /// Definitions used directly in a scope (not inside its composite Cards).
  private func definitions(in scope: [String]) -> [String] {
    var seen: [String] = []
    for c in session.cards(in: scope) where c["canvas"] == nil && c["project"] == nil {
      if let d = c["definition"]?.string, !seen.contains(d), session.definitions[d] != nil { seen.append(d) }
    }
    return seen
  }

  private func composites(in scope: [String]) -> [CardKey] {
    session.cards(in: scope).compactMap { c in
      guard c["canvas"] != nil || c["project"] != nil, let id = c["id"]?.string else { return nil }
      return key(scope + [id])
    }
  }

  /// A composite Card is shown when it (or anything inside) matches.
  private func compositeMatches(_ k: CardKey) -> Bool {
    if search.isEmpty || session.displayName(k).localizedCaseInsensitiveContains(search) { return true }
    let scope = path(k)
    return definitions(in: scope).contains { matches($0) && origin($0).isEmpty } || composites(in: scope).contains(where: compositeMatches)
  }

  var body: some View {
    let all = session.definitions.keys.sorted()
    let local = all.filter { origin($0).isEmpty }
    let used = Set(local.filter { session.usageCount(ofDefinition: $0) > 0 })
    let imported = Dictionary(grouping: all.filter { !origin($0).isEmpty && matches($0) }, by: origin)
    let rootDefs = definitions(in: []).filter { origin($0).isEmpty && matches($0) }
    let unused = local.filter { !used.contains($0) && matches($0) }
    let topComposites = composites(in: []).filter(compositeMatches)
    if all.isEmpty { Text("定義はありません").foregroundStyle(.secondary) }
    else if rootDefs.isEmpty && unused.isEmpty && topComposites.isEmpty && imported.isEmpty && !search.isEmpty {
      Text("一致する定義はありません").foregroundStyle(.secondary)
    }
    if !rootDefs.isEmpty || !topComposites.isEmpty || !unused.isEmpty {
      DisclosureGroup(isExpanded: binding("origin:", default: !search.isEmpty)) {
        ForEach(topComposites, id: \.self) { k in CompositeNode(tree: self, cardKey: k) }
        ForEach(rootDefs, id: \.self) { id in DefinitionLeaf(tree: self, id: id) }
        if !unused.isEmpty {
          DisclosureGroup(isExpanded: binding("unused:", default: !search.isEmpty)) {
            ForEach(unused, id: \.self) { id in DefinitionLeaf(tree: self, id: id) }
          } label: {
            Label("配置されていない定義", systemImage: "tray").font(.callout).foregroundStyle(.secondary)
          }
          .selectionDisabled()
        }
      } label: {
        Label(String(localized: "このプロジェクト"), systemImage: "folder").font(.callout.weight(.medium))
          .help("このプロジェクトの定義。複合 Card（Card in Card）は中の定義を内包しています。")
      }
      .selectionDisabled()
    }
    ForEach(imported.keys.sorted(), id: \.self) { o in
      DisclosureGroup(isExpanded: binding("origin:" + o, default: !search.isEmpty)) {
        ForEach(imported[o] ?? [], id: \.self) { id in DefinitionLeaf(tree: self, id: id) }
      } label: {
        Label(o, systemImage: "square.and.arrow.down.on.square").font(.callout.weight(.medium))
          .help("取り込み元: \(o)（\(imported[o]?.count ?? 0) 件）")
      }
      .selectionDisabled()
    }
  }

  func binding(_ key: String, default value: Bool) -> Binding<Bool> {
    Binding(get: { value || expanded.contains(key) }, set: { if $0 { expanded.insert(key) } else { expanded.remove(key) } })
  }

  /// A composite Card: its nested composite Cards and the definitions inside.
  struct CompositeNode: View {
    let tree: DefinitionTree
    let cardKey: CardKey

    var body: some View {
      let s = tree.session
      let scope = path(cardKey)
      let inner = tree.composites(in: scope).filter(tree.compositeMatches)
      let defs = tree.definitions(in: scope).filter(tree.matches)
      let external = s.instance(cardKey)?["project"] != nil
      if inner.isEmpty && defs.isEmpty {
        row(external: external)
      } else {
        DisclosureGroup(isExpanded: tree.binding("comp:" + cardKey, default: !tree.search.isEmpty)) {
          ForEach(inner, id: \.self) { k in CompositeNode(tree: tree, cardKey: k) }
          ForEach(defs, id: \.self) { id in DefinitionLeaf(tree: tree, id: id) }
        } label: { row(external: external) }
        .selectionDisabled()
      }
    }

    private func row(external: Bool) -> some View {
      HStack(spacing: 6) {
        Image(systemName: "square.stack.3d.up").foregroundStyle(Color.accentColor).frame(width: 16)
        VStack(alignment: .leading, spacing: 1) {
          Text(tree.session.displayName(cardKey)).lineLimit(1)
          Text(external ? String(localized: "複合 Card · 別プロジェクト") : String(localized: "複合 Card")).font(.caption2).foregroundStyle(.secondary)
        }
        Spacer()
      }
      .contentShape(Rectangle())
      .contextMenu { Button("Canvas で表示") { tree.onReveal(cardKey) } }
      .help("Card in Card。中で使う定義を内包しています（ダブルクリックで Canvas に表示）")
      .onTapGesture(count: 2) { tree.onReveal(cardKey) }
      .selectionDisabled()
    }
  }

  /// One definition (a leaf); its uses are in its menu.
  struct DefinitionLeaf: View {
    let tree: DefinitionTree
    let id: String

    var body: some View {
      let s = tree.session
      let def = s.definitions[id]
      let uses = s.instances(ofDefinition: id)
      HStack(spacing: 6) {
        Image(systemName: def?["symbol"]?.string ?? definitionKind(def).symbol).foregroundStyle(.secondary).frame(width: 16)
        VStack(alignment: .leading, spacing: 1) {
          Text(def?["name"]?.string ?? id).lineLimit(1)
          Text(uses.isEmpty ? String(localized: "\(definitionKind(def).label) · 未配置") : String(localized: "\(definitionKind(def).label) · \(uses.count) 枚で使用"))
            .font(.caption2).foregroundStyle(.secondary)
        }
        Spacer()
        if s.definitionEdits[id] != nil {
          Image(systemName: "pencil.circle.fill").foregroundStyle(.orange).help("未保存の変更")
        }
      }
      .contentShape(Rectangle())
      .draggable(DefinitionDrag.prefix + id)
      .onTapGesture(count: 2) { tree.onPlace(id) }
      .help("ダブルクリックまたは Canvas へドラッグして配置。定義の変更は使用中の \(uses.count) 枚すべてに反映されます。")
      .contextMenu {
        Button("Canvas に配置") { tree.onPlace(id) }.disabled(!s.isEditable)
        Button("編集…") { tree.onEdit(id) }
        Menu("使用箇所（\(uses.count)）") {
          if uses.isEmpty { Text("配置されていません") }
          ForEach(uses, id: \.self) { k in
            Button(s.displayName(k) + "  ·  " + (path(k).dropLast().isEmpty ? String(localized: "最上位") : path(k).dropLast().joined(separator: " / "))) {
              tree.onReveal(k)
            }
          }
        }
      }
      .selectionDisabled()
    }
  }
}

func definitionKind(_ def: JSONValue?) -> CardKind {
  if let t = def?["template"]?.string, let k = CardKind(rawValue: t) { return k }
  return def?["language"] == nil ? .draft : .code
}

/// Dragging a definition from the sidebar onto the canvas places a Card
/// (carried as a prefixed string).
enum DefinitionDrag {
  static let prefix = "tsugumi-definition:"
  static func id(_ s: String) -> String? { s.hasPrefix(prefix) ? String(s.dropFirst(prefix.count)) : nil }
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
      if kind == .subgraph, session.instance(cardKey)?["canvas"] != nil {
        Button("開く") { tab.enter(path(cardKey)) }
          .buttonStyle(.borderless).font(.caption)
          .help("子 Workflow を開いて中を編集")
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
    ("filesystem", .bool(false)), ("sandbox", .bool(true)), ("canvasEnabled", .bool(false)),
    ("timeoutSeconds", .null),
  ] + (code ? [("buildTimeoutSeconds", .null), ("inputMode", .string("all"))] : [])
  for (k, v) in defaults where c[k] == nil { c[k] = v }
  return c
}
