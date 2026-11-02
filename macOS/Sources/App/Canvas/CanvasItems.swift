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
    Button("グループ化（見た目だけ）") {
      if !tab.selection.contains(cardKey) { tab.selection = [cardKey] }
      onGroup()
    }
    Button("子 Workflow にまとめる（入出力のある Card）") {
      if let k = session.makeSubgraph(targets, in: tab.scope) { tab.selection = [k] }
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
        Image(systemName: "chart.xyaxis.line").font(.caption).foregroundStyle(.purple)
        Text(session.displayName(cardKey)).font(.caption.bold()).lineLimit(1)
        if let d = session.displays[cardKey] {
          Text("Run \(String(d.runId.prefix(6))) · 実行 #\(d.invocation)").font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
            .help("Run \(d.runId) の中で、この Card の \(d.invocation) 回目の実行の結果")
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
    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.purple.opacity(0.45)))
    .overlay(alignment: .leading) {
      // Anchor of the display link from the Card (not a computation port).
      Circle().fill(Color.purple.opacity(0.75)).frame(width: 7, height: 7).offset(x: -3.5)
    }
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
    case .table(let columns, let rows)?:
      ScrollView([.horizontal, .vertical]) {
        Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 3) {
          GridRow {
            ForEach(Array(columns.enumerated()), id: \.offset) { _, c in Text(c).font(.caption.bold()) }
          }
          Divider()
          ForEach(Array(rows.prefix(2000).enumerated()), id: \.offset) { _, r in
            GridRow {
              ForEach(Array(r.enumerated()), id: \.offset) { _, cell in
                Text(cell).font(.system(size: large ? 12 : 10, design: .monospaced)).textSelection(.enabled)
              }
            }
          }
        }
        .padding(4)
      }
    case .image(_, let data, let alt)?:
      if let img = NSImage(data: data) {
        Image(nsImage: img).resizable().interpolation(.none).scaledToFit()
          .accessibilityLabel(alt)
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else {
        Text("画像を表示できません").font(.caption).foregroundStyle(.secondary)
      }
    case .points(let x, let y, let z, let colors)?:
      PointCloudView(x: x, y: y, z: z, colors: colors)
    case .module(let name, let json)?:
      ModuleDisplayView(name: name, json: json, large: large)
    case nil:
      ContentUnavailableView("まだ結果がありません", systemImage: "chart.xyaxis.line", description: Text("実行すると表示されます。"))
        .font(.caption)
    }
  }
}

/// A 3D point cloud: drag to rotate, scroll or pinch to zoom.
struct PointCloudView: View {
  let x: [Float], y: [Float], z: [Float], colors: [UInt32]
  @State private var yaw = 0.6
  @State private var pitch = 0.4
  @State private var scale = 1.0
  @State private var dragStart: (Double, Double)?

  var body: some View {
    Canvas { ctx, size in
      guard !x.isEmpty else { return }
      let cx = Double(x.reduce(0, +)) / Double(x.count), cy = Double(y.reduce(0, +)) / Double(y.count), cz = Double(z.reduce(0, +)) / Double(z.count)
      var extent = 1e-9
      for i in x.indices { extent = max(extent, abs(Double(x[i]) - cx), abs(Double(y[i]) - cy), abs(Double(z[i]) - cz)) }
      let s = min(size.width, size.height) * 0.42 / extent * scale
      let (cyw, syw, cp, sp) = (cos(yaw), sin(yaw), cos(pitch), sin(pitch))
      let step = max(1, x.count / 200_000)
      for i in stride(from: 0, to: x.count, by: step) {
        let px = Double(x[i]) - cx, py = Double(y[i]) - cy, pz = Double(z[i]) - cz
        let rx = px * cyw + pz * syw
        let rz = -px * syw + pz * cyw
        let ry = py * cp - rz * sp
        let sx = size.width / 2 + rx * s, sy = size.height / 2 - ry * s
        let c = i < colors.count ? colors[i] : 0x4A90E2
        let color = Color(red: Double((c >> 16) & 0xFF) / 255, green: Double((c >> 8) & 0xFF) / 255, blue: Double(c & 0xFF) / 255)
        ctx.fill(Path(ellipseIn: CGRect(x: sx - 1.5, y: sy - 1.5, width: 3, height: 3)), with: .color(color))
      }
    }
    .background(Color(nsColor: .textBackgroundColor))
    .contentShape(Rectangle())
    .gesture(DragGesture().onChanged { v in
      if dragStart == nil { dragStart = (yaw, pitch) }
      yaw = dragStart!.0 + v.translation.width / 120
      pitch = max(-1.5, min(1.5, dragStart!.1 + v.translation.height / 120))
    }.onEnded { _ in dragStart = nil })
    .gesture(MagnifyGesture().onChanged { scale = max(0.2, min(8, $0.magnification)) })
    .overlay(alignment: .bottomTrailing) {
      Text("\(x.count) 点 · ドラッグで回転").font(.caption2).foregroundStyle(.secondary).padding(4)
    }
    .accessibilityLabel(String(localized: "点群 \(x.count) 点"))
  }
}

/// Data for a UI Canvas module. Known modules are drawn natively; any other
/// module shows its data as formatted JSON.
struct ModuleDisplayView: View {
  let name: String
  let json: String
  var large = false

  var body: some View {
    let value = try? JSONValue.parse(json)
    VStack(alignment: .leading, spacing: 4) {
      if name == "histogram", let bins = value?["bins"]?.array?.compactMap(\.double), !bins.isEmpty {
        let top = bins.max() ?? 1
        GeometryReader { g in
          HStack(alignment: .bottom, spacing: 2) {
            ForEach(Array(bins.enumerated()), id: \.offset) { _, b in
              RoundedRectangle(cornerRadius: 2).fill(Color.accentColor.opacity(0.7))
                .frame(height: max(1, g.size.height * b / max(top, 1e-9)))
                .help("\(b)")
            }
          }
          .frame(maxHeight: .infinity, alignment: .bottom)
        }
      } else {
        Text("モジュール \(name)").font(.caption.bold()).foregroundStyle(.secondary)
        ScrollView {
          Text(value?.serialized() ?? json).font(.system(size: large ? 12 : 10, design: .monospaced)).textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
      }
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
