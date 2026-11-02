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
          let cards = session.cards(in: tab.scope).compactMap { $0["id"]?.string }
          if cards.isEmpty { Text("Card はありません").foregroundStyle(.secondary) }
          ForEach(cards, id: \.self) { id in
            VisibleCardRow(session: session, tab: tab, cardKey: key(tab.scope + [id]))
              .tag(key(tab.scope + [id]))
          }
        }
      }
      .listStyle(.sidebar)
      Divider()
      Button {
        openWindow(id: "workflow-settings", value: ProjectRef(path: session.identity.url.path))
      } label: {
        Label("Workflow 設定", systemImage: "gearshape").frame(maxWidth: .infinity, alignment: .leading)
      }
      .buttonStyle(.borderless)
      .padding(10)
    }
    .sheet(isPresented: $importing) { ImportDefinitionsSheet(session: session) }
  }

  @ViewBuilder private var definitions: some View {
    TextField("定義を検索", text: $defSearch).textFieldStyle(.roundedBorder)
    let ids = session.definitions.keys.sorted().filter { id in
      defSearch.isEmpty || id.localizedCaseInsensitiveContains(defSearch) ||
        (session.definitions[id]?["name"]?.string ?? "").localizedCaseInsensitiveContains(defSearch)
    }
    if ids.isEmpty { Text(defSearch.isEmpty ? "定義はありません" : "一致する定義はありません").foregroundStyle(.secondary) }
    ForEach(ids, id: \.self) { id in
      let def = session.definitions[id]
      HStack {
        Image(systemName: definitionKind(def).symbol).foregroundStyle(.secondary).frame(width: 16)
        VStack(alignment: .leading, spacing: 1) {
          Text(def?["name"]?.string ?? id).lineLimit(1)
          Text("\(definitionKind(def).label) · \(session.usageCount(ofDefinition: id)) 枚で使用")
            .font(.caption2).foregroundStyle(.secondary)
        }
        Spacer()
        if session.definitionEdits[id] != nil {
          Image(systemName: "pencil.circle.fill").foregroundStyle(.orange).help("未保存の変更")
        }
      }
      .contentShape(Rectangle())
      .draggable(DefinitionDrag.prefix + id)
      .onTapGesture(count: 2) { place(id) }
      .help("ダブルクリックまたは Canvas へドラッグして配置")
      .contextMenu {
        Button("Canvas に配置") { place(id) }.disabled(!session.isEditable)
        Button("編集…") { openWindow(id: "definitions", value: ProjectRef(path: session.identity.url.path)) }
      }
      .selectionDisabled()
    }
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

/// A Card of the current scope: name and run state; selection and collapse
/// follow the canvas.
struct VisibleCardRow: View {
  let session: ProjectSession
  let tab: WorkflowTab
  let cardKey: CardKey

  var body: some View {
    let state = session.cardStates[cardKey]?.state
    let errors = session.resolution[cardKey]?.diagnostics.filter(\.error).count ?? 0
    HStack(spacing: 6) {
      Button {
        session.setCollapsed([cardKey], !session.collapsed(cardKey))
      } label: {
        Image(systemName: session.collapsed(cardKey) ? "chevron.right" : "chevron.down").font(.caption2)
      }
      .buttonStyle(.borderless)
      .help(session.collapsed(cardKey) ? "Canvas で展開" : "Canvas で折りたたむ（実行には影響しません）")
      Image(systemName: session.kind(cardKey).symbol).foregroundStyle(.secondary).frame(width: 14)
      Text(session.displayName(cardKey)).lineLimit(1)
      Spacer()
      if errors > 0 {
        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow).help("問題 \(errors) 件")
      }
      if let state {
        Label(state.label, systemImage: state.symbol)
          .labelStyle(.iconOnly)
          .foregroundStyle(state.color)
          .help(state.label)
      }
    }
    .accessibilityElement(children: .combine)
    .accessibilityLabel("\(session.displayName(cardKey)), \(state?.label ?? "未実行")")
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
      Text("定義とソースをコピーし、ID が重なる場合は新しい ID を割り当てます。Canvas への配置は別に行います。")
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
        message = "\(chosen.count) 件をコピーしました。" + (renamed.isEmpty ? "" : " 新しい ID: " + renamed.map { "\($0.key) → \($0.value)" }.joined(separator: ", "))
      } catch {
        message = "コピーできませんでした: \(error)"
      }
      working = false
    }
  }
}
