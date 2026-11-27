import AppKit
import SwiftUI
import TsugumiRPC
import TsugumiViewPresentation

/// Right-click menu of a Card.
struct CardMenu: View {
  @Environment(\.openWindow) private var openWindow
  let workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab
  let cardKey: CardKey
  let onGroup: () -> Void

  var body: some View {
    let ref = CardRef(path: session.identity.url.path, card: cardKey)
    let kind = session.kind(cardKey)
    let targets = tab.selection.contains(cardKey) ? tab.selection : [cardKey]
    if kind == .subgraph {
      Button("開く") { tab.enter(path(cardKey)) }
      Button("新しいタブで開く") { workspace.openTab(path(cardKey)) }
      Button("別ウインドウで開く") { openWindow(id: "workflow", value: WorkflowRef(path: ref.path, scope: path(cardKey))) }
      Divider()
    }
    if kind == .draft {
      Button("名前と内容を書く") {
        NotificationCenter.default.post(name: .canvasWriteDraft, object: tab.id, userInfo: ["card": cardKey])
      }
      .disabled(!session.isEditable)
    }
    Button("設定…") { openWindow(id: "card", value: ref) }
    Button("コメント…") { openWindow(id: "card", value: CardRef(path: ref.path, card: cardKey, category: .notes)) }
    Button("実行履歴…") { openWindow(id: "executions", value: ref) }
    Button("成果物…") { openWindow(id: "artifacts", value: ref) }
    Button("ログを表示") { workspace.showLog(for: cardKey) }
    // The Card's view panels: folded away from the canvas and back (they stay open meanwhile).
    ForEach(session.cardViews.panelOpenings.filter { key($0.card) == cardKey }, id: \.id) { view in
      if workspace.foldedCardViews.contains(view.id) {
        Button("表示「\(view.name)」を広げる") { workspace.foldedCardViews.remove(view.id) }
      } else {
        Button("表示「\(view.name)」を折りたたむ") { workspace.foldedCardViews.insert(view.id) }
      }
    }
    Menu("Skill を実行") {
      ForEach(AppModel.shared.chat(for: session).skills.filter(\.scoped)) { skill in
        Button("$\(skill.name) (\(skill.scope.joined(separator: ", ")))") {
          workspace.showChat = true
          AppModel.shared.chat(for: session).send("$\(skill.name)", selection: targets.sorted(), scopedSkill: skill.name)
        }.disabled(AppModel.shared.chat(for: session).busy)
      }
    }
    Button("チャットに参照を追加") {
      for k in targets.sorted() { AppModel.shared.chat(for: session).addReference(.card(k)) }
      workspace.showChat = true
    }
    Divider()
    Button("ここから下流を実行") { RunActions.run(session, card: cardKey, propagate: true) }
      .disabled(!session.connected || session.runActive || kind == .invalid)
    Button("この Card だけ実行（最後の入力）") { RunActions.run(session, card: cardKey, propagate: false) }
      .disabled(!session.connected || session.runActive || kind == .invalid)
    Divider()
    Button("複製") { Task { tab.selection = await session.duplicate(targets, in: tab.scope) } }
      .disabled(!session.isEditable)
    Button(session.collapsed(cardKey) ? "展開" : "折りたたむ") { session.setCollapsed(targets, !session.collapsed(cardKey)) }
    if targets.contains(where: { session.hasPortOrder($0) }) {
      Button("ポートの並びを元に戻す") { session.resetPortOrder(targets) }
        .disabled(!session.isEditable)
    }
    Button("グループ化（見た目だけ）") {
      if !tab.selection.contains(cardKey) { tab.selection = [cardKey] }
      onGroup()
    }
    Button("Card in Card にまとめる（新しい external プロジェクト）") {
      Task { if let k = await session.makeSubgraph(targets, in: tab.scope) { tab.selection = [k] } }
    }
    .disabled(!session.isEditable)
    Divider()
    Button("入力を切断") { session.disconnect(cardKey, inputs: true, in: tab.scope) }.disabled(!session.isEditable)
    Button("出力を切断") { session.disconnect(cardKey, inputs: false, in: tab.scope) }.disabled(!session.isEditable)
    Button("削除", role: .destructive) {
      session.deleteCards(targets, in: tab.scope)
      tab.selection.subtract(targets)
    }
    .disabled(!session.isEditable)
  }
}

func noteColor(_ id: String?) -> Color {
  switch id {
  case "blue": return .blue
  case "green": return .green
  case "pink": return .pink
  case "gray": return .gray
  default: return .yellow
  }
}

/// Visual grouping frame drawn behind its Cards.
struct GroupFrameView: View {
  let session: ProjectSession
  let group: JSONValue
  let scope: [String]
  @Environment(\.canvasZoom) private var z

  var body: some View {
    let color = noteColor(group["color"]?.string)
    let outputs = (group["outputs"]?.array ?? []).compactMap { o -> String? in
      guard let c = o["card"]?.string, let p = o["port"]?.string else { return nil }
      return "\(session.displayName(key(scope + [c]))).\(p)"
    }
    ZStack(alignment: .topLeading) {
      RoundedRectangle(cornerRadius: 12 * z).fill(color.opacity(0.07))
      RoundedRectangle(cornerRadius: 12 * z).strokeBorder(color.opacity(0.5), style: StrokeStyle(lineWidth: 1.2, dash: [6, 4]))
      HStack(spacing: 6 * z) {
        Image(systemName: "rectangle.3.group").font(.system(size: 10 * z))
        Text(group["name"]?.string ?? String(localized: "グループ")).font(.system(size: 12 * z, weight: .semibold))
        Spacer()
      }
      .foregroundStyle(color)
      .padding(.horizontal, 10 * z).padding(.top, 5 * z)
      if !outputs.isEmpty {
        VStack(alignment: .trailing, spacing: 2 * z) {
          ForEach(outputs, id: \.self) { o in
            Label(o, systemImage: "arrow.right.circle").font(.system(size: 10 * z))
              .padding(.horizontal, 6 * z).padding(.vertical, 2 * z)
              .background(color.opacity(0.15), in: Capsule())
          }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
        .padding(6 * z)
        .help("グループの公開出力")
      }
    }
    .accessibilityLabel("グループ \(group["name"]?.string ?? "")")
  }
}

/// Markdown sticky note (with TeX math); it does not take part in runs. Its
/// text size is its own (`fontSize` in the layout).
struct StickyNoteView: View {
  let note: JSONValue
  var selected = false
  let onResize: (CGSize) -> Void
  @Environment(\.canvasZoom) private var z

  static func fontSize(_ note: JSONValue) -> CGFloat { CGFloat(note["fontSize"]?.double ?? 13) }

  var body: some View {
    // Cut at the note's size (no scroll bar on the canvas; resize to see more).
    MarkdownView(text: note["text"]?.string ?? "", fontSize: Self.fontSize(note) * z)
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
      .clipped()
    .padding(10 * z)
    .background(noteColor(note["color"]?.string).opacity(0.28), in: RoundedRectangle(cornerRadius: 6 * z))
    .overlay(RoundedRectangle(cornerRadius: 6 * z).strokeBorder(noteColor(note["color"]?.string).opacity(0.5)))
    .overlay { if selected { RoundedRectangle(cornerRadius: 6 * z).strokeBorder(Color.accentColor, lineWidth: 2.4) } }
    .overlay(alignment: .bottomTrailing) { ResizeHandle(onEnd: onResize) }
    .help("ダブルクリックで編集")
  }
}

struct ResizeHandle: View {
  let onEnd: (CGSize) -> Void
  @State private var live: CGSize = .zero

  var body: some View {
    Image(systemName: "arrow.down.right.and.arrow.up.left")
      .font(.system(size: 8))
      .foregroundStyle(.secondary)
      .frame(width: 16, height: 16)
      .contentShape(Rectangle())
      .offset(x: live.width, y: live.height)
      .gesture(DragGesture().onChanged { live = $0.translation }.onEnded { v in
        live = .zero
        onEnd(v.translation)
      })
      .help("ドラッグしてサイズ変更")
  }
}

struct NoteEditor: View {
  @Environment(\.dismiss) private var dismiss
  let session: ProjectSession
  let noteId: String
  @State private var text = ""
  @State private var color = "yellow"
  @State private var width = 240.0
  @State private var height = 120.0
  @State private var fontSize = 13.0

  @State private var edit = InlineParamEdit()

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("付箋を編集").font(.title2.bold())
      HSplitView {
        TextEditor(text: $text).font(.system(size: 13, design: .monospaced)).frame(minWidth: 220)
        ScrollView {
          MarkdownView(text: text, fontSize: fontSize)
            .frame(maxWidth: .infinity, alignment: .leading).padding(8)
        }
        .background(noteColor(color).opacity(0.2))
        .frame(minWidth: 200)
      }
      .frame(height: 220)
      Text("Markdown（見出し・太字・リスト・表・コード）と数式（$…$、$$…$$）を使えます。付箋は実行には参加しません。").font(.caption).foregroundStyle(.secondary)
      Stepper("文字の大きさ \(Int(fontSize))", value: $fontSize, in: 8...48, step: 1)
      Picker("色", selection: $color) {
        ForEach(NoteColor.all, id: \.id) { c in Text(c.label).tag(c.id) }
      }
      .pickerStyle(.segmented)
      HStack {
        Stepper("幅 \(Int(width))", value: $width, in: 120...800, step: 20)
        Stepper("高さ \(Int(height))", value: $height, in: 60...600, step: 20)
      }
      RevisionEditNotice(edit: edit.revision, text: { value }, overwrite: { apply(overwrite: true) }, reload: load)
      HStack {
        Button("削除", role: .destructive) { session.removeNote(noteId); dismiss() }
        Spacer()
        Button("キャンセル") { dismiss() }
        Button("適用") { apply() }.keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 560)
    .onAppear(perform: load)
  }

  private func load() {
    let n = session.note(noteId)
    text = n?["text"]?.string ?? ""
    color = n?["color"]?.string ?? "yellow"
    width = n?["width"]?.double ?? 240
    height = n?["height"]?.double ?? 120
    fontSize = n?["fontSize"]?.double ?? 13
    let file = (session.projectPrefix(session.scopeHolding("notes", noteId) ?? []) ?? "") + "layout.bin"
    edit.beginField(value, session: session, file: file, selector: [.string("notes"), .object([("id", .string(noteId))])], value: n)
  }

  private var value: String { JSONValue.array([.string(text), .string(color), .real(width), .real(height), .real(fontSize)]).serialized() }
  private func apply(overwrite: Bool = false) {
    Task {
      let ok = await edit.commitField(session, text: value, overwrite: overwrite) { n in
        n?["text"] = .string(text)
        n?["color"] = .string(color)
        n?["width"] = .real(width)
        n?["height"] = .real(height)
        n?["fontSize"] = fontSize == 13 ? nil : .real(fontSize)
      }
      if ok { dismiss() }
    }
  }
}

/// Edits a group: name, members and the outputs shown on the frame.
struct GroupEditor: View {
  @Environment(\.dismiss) private var dismiss
  let session: ProjectSession
  let scope: [String]
  let groupId: String
  @State private var name = ""
  @State private var color = "blue"
  @State private var members: Set<String> = []
  @State private var outputs: Set<String> = []  // "card\tport"

  @State private var edit = InlineParamEdit()

  var body: some View {
    let ids = session.cards(in: scope).compactMap { $0["id"]?.string }
    VStack(alignment: .leading, spacing: 12) {
      Text("グループを編集").font(.title2.bold())
      TextField("名前", text: $name)
      Picker("色", selection: $color) {
        ForEach(NoteColor.all, id: \.id) { c in Text(c.label).tag(c.id) }
      }
      .pickerStyle(.segmented)
      HStack(alignment: .top) {
        VStack(alignment: .leading) {
          Text("所属する Card").font(.headline)
          List(ids, id: \.self) { id in
            Toggle(session.displayName(key(scope + [id])), isOn: Binding(
              get: { members.contains(id) }, set: { if $0 { members.insert(id) } else { members.remove(id) } }))
          }
        }
        VStack(alignment: .leading) {
          Text("公開する出力").font(.headline)
          List {
            ForEach(members.sorted(), id: \.self) { id in
              ForEach(session.ports(key(scope + [id])).outputs, id: \.self) { p in
                let tag = "\(id)\t\(p)"
                Toggle("\(session.displayName(key(scope + [id]))).\(p)", isOn: Binding(
                  get: { outputs.contains(tag) }, set: { if $0 { outputs.insert(tag) } else { outputs.remove(tag) } }))
              }
            }
          }
        }
      }
      .frame(height: 240)
      Text("グループは見た目のまとまりです。実行順序は変わらず、解除しても Card は残ります。").font(.caption).foregroundStyle(.secondary)
      RevisionEditNotice(edit: edit.revision, text: { value }, overwrite: { apply(overwrite: true) }, reload: load)
      HStack {
        Button("グループを解除", role: .destructive) { session.ungroup(groupId); dismiss() }
        Spacer()
        Button("キャンセル") { dismiss() }
        Button("適用") { apply() }.keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 600)
    .onAppear(perform: load)
  }

  private func load() {
    let g = session.groups(in: scope).first { $0["id"]?.string == groupId }
    name = g?["name"]?.string ?? ""
    color = g?["color"]?.string ?? "blue"
    members = Set((g?["cards"]?.array ?? []).compactMap { $0.string })
    outputs = Set((g?["outputs"]?.array ?? []).compactMap { o in
      guard let c = o["card"]?.string, let p = o["port"]?.string else { return nil }
      return "\(c)\t\(p)"
    })
    let file = (session.projectPrefix(scope) ?? "") + "layout.bin"
    edit.beginField(value, session: session, file: file, selector: [.string("groups"), .object([("id", .string(groupId))])], value: g)
  }

  private var value: String { JSONValue.array([.string(name), .string(color), .array(members.sorted().map { .string($0) }), .array(outputs.sorted().map { .string($0) })]).serialized() }
  private func apply(overwrite: Bool = false) {
    Task {
      let ok = await edit.commitField(session, text: value, overwrite: overwrite) { g in
        g?["name"] = .string(name)
        g?["color"] = .string(color)
        g?["cards"] = .array(members.sorted().map { .string($0) })
        let outs: [JSONValue] = outputs.sorted().compactMap { t in
          let parts = t.split(separator: "\t").map(String.init)
          guard parts.count == 2, members.contains(parts[0]) else { return nil }
          return .object([("card", .string(parts[0])), ("port", .string(parts[1]))])
        }
        g?["outputs"] = outs.isEmpty ? nil : .array(outs)
      }
      if ok { dismiss() }
    }
  }
}
