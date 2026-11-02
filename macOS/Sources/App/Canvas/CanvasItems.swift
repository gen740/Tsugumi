import AppKit
import SwiftUI
import TsugumiRPC
import TsugumiRenderer

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
    if kind == .subgraph, session.instance(cardKey)?["canvas"] != nil {
      Button("開く") { tab.enter(path(cardKey)) }
      Button("新しいタブで開く") { workspace.openTab(path(cardKey)) }
      Button("別ウインドウで開く") { openWindow(id: "workflow", value: WorkflowRef(path: ref.path, scope: path(cardKey))) }
      Divider()
    }
    if kind == .draft { Button("Draft を編集…") { openWindow(id: "draft", value: ref) } }
    Button("設定…") { openWindow(id: "card", value: ref) }
    Button("実行履歴…") { openWindow(id: "executions", value: ref) }
    Button("成果物…") { openWindow(id: "artifacts", value: ref) }
    Button("ログを表示") { workspace.showLog(for: cardKey) }
    Button("チャットに参照を追加") {
      for k in targets.sorted() { AppModel.shared.chat(for: session).addReference(.card(k)) }
      workspace.showChat = true
    }
    Divider()
    Button("ここから下流を実行") { RunActions.run(session, card: cardKey, propagate: true) }
      .disabled(!session.connected || session.runActive)
    Button("この Card だけ実行（最後の入力）") { RunActions.run(session, card: cardKey, propagate: false) }
      .disabled(!session.connected || session.runActive)
    Divider()
    Button("複製") { tab.selection = session.duplicate(targets, in: tab.scope) }.disabled(!session.isEditable)
    Button(session.collapsed(cardKey) ? "展開" : "折りたたむ") { session.setCollapsed(targets, !session.collapsed(cardKey)) }
    if session.cardSetting(cardKey, "canvasEnabled")?.bool == true {
      if session.displayFrame(cardKey)?["hidden"]?.bool == true {
        Button("出力表示を戻す") { session.updateDisplayFrame(cardKey) { $0["hidden"] = nil } }
      }
    } else if kind != .subgraph && kind != .draft {
      Button("出力表示を有効にする") {
        session.edit { s in s.updateInstance(cardKey) { $0["canvasEnabled"] = .bool(true) } }
      }
      .disabled(!session.isEditable)
    }
    Button("グループ化") {
      if !tab.selection.contains(cardKey) { tab.selection = [cardKey] }
      onGroup()
    }
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

  var body: some View {
    let color = noteColor(group["color"]?.string)
    let outputs = (group["outputs"]?.array ?? []).compactMap { o -> String? in
      guard let c = o["card"]?.string, let p = o["port"]?.string else { return nil }
      return "\(session.displayName(key(scope + [c]))).\(p)"
    }
    ZStack(alignment: .topLeading) {
      RoundedRectangle(cornerRadius: 12).fill(color.opacity(0.07))
      RoundedRectangle(cornerRadius: 12).strokeBorder(color.opacity(0.5), style: StrokeStyle(lineWidth: 1.2, dash: [6, 4]))
      HStack(spacing: 6) {
        Image(systemName: "rectangle.3.group").font(.caption)
        Text(group["name"]?.string ?? "グループ").font(.system(size: 12, weight: .semibold))
        Spacer()
      }
      .foregroundStyle(color)
      .padding(.horizontal, 10).padding(.top, 5)
      if !outputs.isEmpty {
        VStack(alignment: .trailing, spacing: 2) {
          ForEach(outputs, id: \.self) { o in
            Label(o, systemImage: "arrow.right.circle").font(.system(size: 10))
              .padding(.horizontal, 6).padding(.vertical, 2)
              .background(color.opacity(0.15), in: Capsule())
          }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
        .padding(6)
        .help("グループの公開出力")
      }
    }
    .accessibilityLabel("グループ \(group["name"]?.string ?? "")")
  }
}

/// Markdown sticky note; it does not take part in runs.
struct StickyNoteView: View {
  let note: JSONValue
  let onResize: (CGSize) -> Void

  var body: some View {
    let text = note["text"]?.string ?? ""
    let md = (try? AttributedString(markdown: text, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))) ?? AttributedString(text)
    ScrollView {
      Text(md).font(.callout).frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled)
    }
    .padding(10)
    .background(noteColor(note["color"]?.string).opacity(0.28), in: RoundedRectangle(cornerRadius: 6))
    .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(noteColor(note["color"]?.string).opacity(0.5)))
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

/// A Card's result in its own movable frame.
struct OutputFrameView: View {
  let session: ProjectSession
  let cardKey: CardKey
  let onExpand: () -> Void
  let onClose: () -> Void
  let onResize: (CGSize) -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      HStack(spacing: 6) {
        Image(systemName: "chart.xyaxis.line").font(.caption)
        Text(session.displayName(cardKey)).font(.caption.bold()).lineLimit(1)
        if let d = session.displays[cardKey] {
          Text("#\(d.invocation)").font(.caption2).foregroundStyle(.secondary)
        }
        Spacer()
        Button(action: onExpand) { Image(systemName: "arrow.up.left.and.arrow.down.right") }
          .buttonStyle(.borderless).help("拡大して表示")
        Button(action: onClose) { Image(systemName: "xmark") }
          .buttonStyle(.borderless).help("枠を削除（Card は残ります）")
      }
      .foregroundStyle(.secondary)
      DisplayContent(display: session.displays[cardKey])
    }
    .padding(8)
    .background(RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .windowBackgroundColor)))
    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.secondary.opacity(0.3)))
    .overlay(alignment: .bottomTrailing) { ResizeHandle(onEnd: onResize) }
  }
}

/// Renders a Display value (text, scalar, Dawn line chart).
struct DisplayContent: View {
  let display: DisplaySnapshot?
  var large = false

  var body: some View {
    switch display?.value {
    case .text(let t)?:
      ScrollView {
        Text(t).font(.system(size: large ? 13 : 11, design: .monospaced)).textSelection(.enabled)
          .frame(maxWidth: .infinity, alignment: .leading)
      }
    case .scalar(let v, let label)?:
      VStack {
        Text(label).font(large ? .title3 : .caption)
        Text(v, format: .number.precision(.significantDigits(8))).font(large ? .system(size: 64).monospacedDigit() : .title.monospacedDigit())
          .textSelection(.enabled)
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
    case .lineChart(let series)?:
      VStack(spacing: 4) {
        DawnLineChart(series: series.map { ChartSeries(id: $0.id, label: $0.label, x: $0.x, y: $0.y) })
          .clipShape(RoundedRectangle(cornerRadius: 4))
        HStack {
          ForEach(Array(series.enumerated()), id: \.offset) { _, s in
            Label("\(s.label) (\(s.x.count))", systemImage: "line.diagonal").font(.caption2)
          }
        }
      }
    case nil:
      ContentUnavailableView("まだ結果がありません", systemImage: "chart.xyaxis.line", description: Text("実行すると表示されます。"))
        .font(.caption)
    }
  }
}

/// Edits a sticky note: Markdown text, color, size.
struct NoteEditor: View {
  @Environment(\.dismiss) private var dismiss
  let session: ProjectSession
  let noteId: String
  @State private var text = ""
  @State private var color = "yellow"
  @State private var width = 240.0
  @State private var height = 120.0

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("付箋を編集").font(.title2.bold())
      HSplitView {
        TextEditor(text: $text).font(.system(size: 13, design: .monospaced)).frame(minWidth: 220)
        ScrollView {
          Text((try? AttributedString(markdown: text, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))) ?? AttributedString(text))
            .frame(maxWidth: .infinity, alignment: .leading).padding(8)
        }
        .background(noteColor(color).opacity(0.2))
        .frame(minWidth: 200)
      }
      .frame(height: 220)
      Text("Markdown（太字・斜体・リンク・コード）を使えます。付箋は実行には参加しません。").font(.caption).foregroundStyle(.secondary)
      Picker("色", selection: $color) {
        ForEach(NoteColor.all, id: \.id) { c in Text(c.label).tag(c.id) }
      }
      .pickerStyle(.segmented)
      HStack {
        Stepper("幅 \(Int(width))", value: $width, in: 120...800, step: 20)
        Stepper("高さ \(Int(height))", value: $height, in: 60...600, step: 20)
      }
      HStack {
        Button("削除", role: .destructive) { session.removeNote(noteId); dismiss() }
        Spacer()
        Button("キャンセル") { dismiss() }
        Button("適用") { apply(); dismiss() }.keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 560)
    .onAppear {
      let n = (session.layout["notes"]?.array ?? []).first { $0["id"]?.string == noteId }
      text = n?["text"]?.string ?? ""
      color = n?["color"]?.string ?? "yellow"
      width = n?["width"]?.double ?? 240
      height = n?["height"]?.double ?? 120
    }
  }

  private func apply() {
    session.updateNote(noteId) { n in
      n["text"] = .string(text)
      n["color"] = .string(color)
      n["width"] = .real(width)
      n["height"] = .real(height)
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
      HStack {
        Button("グループを解除", role: .destructive) { session.ungroup(groupId); dismiss() }
        Spacer()
        Button("キャンセル") { dismiss() }
        Button("適用") { apply(); dismiss() }.keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 600)
    .onAppear {
      let g = session.groups(in: scope).first { $0["id"]?.string == groupId }
      name = g?["name"]?.string ?? ""
      color = g?["color"]?.string ?? "blue"
      members = Set((g?["cards"]?.array ?? []).compactMap { $0.string })
      outputs = Set((g?["outputs"]?.array ?? []).compactMap { o in
        guard let c = o["card"]?.string, let p = o["port"]?.string else { return nil }
        return "\(c)\t\(p)"
      })
    }
  }

  private func apply() {
    session.updateGroup(groupId) { g in
      g["name"] = .string(name)
      g["color"] = .string(color)
      g["cards"] = .array(members.sorted().map { .string($0) })
      let outs: [JSONValue] = outputs.sorted().compactMap { t in
        let parts = t.split(separator: "\t").map(String.init)
        guard parts.count == 2, members.contains(parts[0]) else { return nil }
        return .object([("card", .string(parts[0])), ("port", .string(parts[1]))])
      }
      g["outputs"] = outs.isEmpty ? nil : .array(outs)
    }
  }
}
