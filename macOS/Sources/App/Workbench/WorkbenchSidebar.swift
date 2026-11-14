import AppKit
import SwiftUI
import TsugumiRPC

/// Left column: workflow hierarchy, new Cards (each with a definition folder
/// of its own) and the Cards shown in the current scope.
struct WorkbenchSidebar: View {
  @Environment(\.openWindow) private var openWindow
  @Environment(\.openSettings) private var openSettings
  @Bindable var workspace: Workspace
  let session: ProjectSession
  @Bindable var tab: WorkflowTab
  @State private var defSearch = ""
  /// Expanded tree nodes; kept across searches and reloads.
  @State private var expanded: Set<String> = ["origin:"]
  @State private var importError: String?

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
            Button { importProject() } label: { Image(systemName: "square.and.arrow.down.on.square") }
              .buttonStyle(.borderless)
              .disabled(!session.isEditable)
              .help("他のプロジェクトを取り込む（externals にコピーして Card in Card として置きます）")
              .accessibilityLabel("プロジェクトを取り込む")
              .padding(.trailing, 10)
          }
        }
        Section("表示中の Card") {
          VisibleCards(session: session, tab: tab)
        }
      }
      .listStyle(.sidebar)
      .safeAreaInset(edge: .bottom, spacing: 0) {
        // The project's versions, and the settings window (⌘,) at its settings.
        HStack {
          Button {
            openWindow(id: "versions", value: ProjectRef(path: session.identity.url.path))
          } label: { Image(systemName: "point.3.connected.trianglepath.dotted").font(.system(size: 13)).frame(width: 26, height: 22) }
            .buttonStyle(.borderless)
            .help("版の履歴（git、⌥⌘Y）")
            .accessibilityLabel("版の履歴")
            .guideTarget("versions")
          Button {
            ProjectSettingsPane.open(session, with: openSettings)
          } label: { Image(systemName: "gearshape").font(.system(size: 13)).frame(width: 26, height: 22) }
            .buttonStyle(.borderless)
            .help("このプロジェクトの設定（設定ウインドウの「このプロジェクト」）")
            .accessibilityLabel("プロジェクトの設定")
            .guideTarget("project_settings")
          Spacer()
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
      }
    }
    .alert("取り込めませんでした", isPresented: Binding(get: { importError != nil }, set: { if !$0 { importError = nil } })) {
      Button("OK", role: .cancel) {}
    } message: { Text(importError ?? "") }
  }

  /// Externals of this project: their definitions (copied into the scope as
  /// new Cards) and the external itself (a new Card in Card, a copy of its own).
  @ViewBuilder private var definitions: some View {
    TextField("検索", text: $defSearch).textFieldStyle(.roundedBorder)
    let exts = session.effectiveExternals.keys.filter { $0.split(separator: "/").count == 2 }.sorted()
    if exts.isEmpty {
      Text("externals にプロジェクトがありません。取り込みボタンから他のプロジェクトを取り込めます。組み込みの Card は Canvas の右クリックか + から追加します。")
        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        .selectionDisabled()
    }
    ForEach(exts, id: \.self) { prefix in
      let name = prefix.split(separator: "/").last.map(String.init) ?? prefix
      let ids = (session.external(prefix)?.definitions.keys.sorted() ?? [])
        .filter { defSearch.isEmpty || $0.localizedCaseInsensitiveContains(defSearch) || name.localizedCaseInsensitiveContains(defSearch) }
      DisclosureGroup(isExpanded: Binding(get: { expanded.contains(prefix) || !defSearch.isEmpty },
                                          set: { if $0 { expanded.insert(prefix) } else { expanded.remove(prefix) } })) {
        row(symbol: CardKind.subgraph.symbol, title: String(localized: "\(name) 全体（Card in Card）"),
            drag: DefinitionDrag.externalPrefix + prefix,
            help: String(localized: "このプロジェクト全体のコピーを Card in Card として置きます")) {
          Task { await placeExternal(prefix) }
        }
        ForEach(ids, id: \.self) { id in
          let d = session.external(prefix)?.definitions[id]
          row(symbol: d?.link != nil ? CardKind.subgraph.symbol : definitionKind(d?.card).symbol, title: session.displayName(key([id])),
              drag: DefinitionDrag.externalDefinitionPrefix + prefix + "\n" + id,
              help: String(localized: "定義をこのプロジェクトの definitions にコピーして Card を置きます")) {
            Task { await placeCopy(prefix, id) }
          }
        }
      } label: {
        Label(name, systemImage: "shippingbox").lineLimit(1)
      }
      .selectionDisabled()
    }
    // Definition folders without a Card (left over).
    let unplaced = session.unplacedDefinitions.filter { defSearch.isEmpty || (session.displayName(key([$0])).localizedCaseInsensitiveContains(defSearch) || $0.localizedCaseInsensitiveContains(defSearch)) }
    if !unplaced.isEmpty {
      DisclosureGroup(isExpanded: Binding(get: { expanded.contains("unplaced") || !defSearch.isEmpty },
                                          set: { if $0 { expanded.insert("unplaced") } else { expanded.remove("unplaced") } })) {
        ForEach(unplaced, id: \.self) { id in
          row(symbol: definitionKind(session.definitions[id]).symbol, title: session.displayName(key([id])), drag: DefinitionDrag.prefix + id,
              help: String(localized: "ダブルクリックまたは Canvas へドラッグして配置")) { place(id) }
            .contextMenu {
              Button("Canvas に配置") { place(id) }.disabled(!session.isEditable)
              Button("削除", role: .destructive) { session.edit { $0.removeDefinition(id) } }.disabled(!session.isEditable)
            }
        }
      } label: {
        Label("配置されていない定義", systemImage: "tray").font(.callout).foregroundStyle(.secondary)
      }
      .selectionDisabled()
    }
  }

  private func row(symbol: String, title: String, drag: String, help: String, add: @escaping () -> Void) -> some View {
    HStack(spacing: 6) {
      Image(systemName: symbol).foregroundStyle(.secondary).frame(width: 16)
      Text(title).lineLimit(1)
      Spacer()
    }
    .contentShape(Rectangle())
    .draggable(drag)
    .onTapGesture(count: 2) { if session.isEditable { add() } }
    .help(help + String(localized: "（ダブルクリックまたは Canvas へドラッグ）"))
    .contextMenu { Button("Canvas に追加") { add() }.disabled(!session.isEditable) }
    .selectionDisabled()
  }

  private var dropPoint: CGPoint {
    let cam = session.cameras[key(tab.scope)] ?? (40, 40, 1)
    return CGPoint(x: (240 - cam.x) / cam.zoom, y: (160 - cam.y) / cam.zoom)
  }

  private func place(_ id: String) {
    guard session.isEditable, tab.scope.isEmpty else { return }
    tab.selection = [session.placeDefinition(id, at: dropPoint)]
  }

  private func placeCopy(_ prefix: String, _ id: String) async {
    if let k = await session.placeDefinitionCopy(prefix: prefix, id: id, in: tab.scope, at: dropPoint) { tab.selection = [k] }
  }

  private func placeExternal(_ prefix: String) async {
    if let k = await session.placeExternalCopy(from: prefix, in: tab.scope, at: dropPoint) { tab.selection = [k] }
  }

  /// Chooses a Tsugumi project folder and takes it in as a Card in Card.
  private func importProject() {
    let p = NSOpenPanel()
    p.canChooseDirectories = true
    p.canChooseFiles = false
    p.prompt = String(localized: "取り込む")
    p.message = String(localized: "取り込む Tsugumi のプロジェクト（tsugumi.json のあるフォルダ）を選んでください。externals にコピーされ、元のフォルダとは連動しません。")
    guard p.runModal() == .OK, let url = p.url else { return }
    Task {
      do { tab.selection = [try await session.importProject(from: url, in: tab.scope, at: dropPoint)] }
      catch { importError = (error as? RPCFailure)?.diagnostic ?? "\(error)" }
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
  /// A definition of an external ("<folder>\n<id>"), copied as a new Card.
  static let externalDefinitionPrefix = "tsugumi-external-definition:"
  /// An external itself, copied as a new Card in Card.
  static let externalPrefix = "tsugumi-external:"
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
/// Card has an explicit "Open". Collapsing is done on the Card itself.
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
