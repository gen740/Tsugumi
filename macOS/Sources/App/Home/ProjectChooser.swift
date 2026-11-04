import AppKit
import SwiftUI
import TsugumiRPC

/// Home (new, existing and recent projects) and the demo list, entered
/// from "デモを見る" with a way back. Both follow the window's size: lists
/// scroll, nothing is cut at the top.
struct ProjectChooser: View {
  @Environment(AppModel.self) private var model
  let onOpen: (URL) -> Void
  @State private var showDemos = false
  @State private var newName = ""
  @State private var showNew = false
  @State private var tag: String?
  @State private var search = ""
  /// Choosing recent projects to take off the list (nil: not choosing).
  @State private var picking: Set<String>?
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  /// The welcome (WelcomeIntro.swift): when it started (nil: not playing),
  /// how far home has come up (0…1), and whether the name and the bird have
  /// reached their places here.
  @State private var welcomeStart: Date?
  @State private var reveal: Double = 1
  @State private var arrived = true
  @State private var homeWord: CGRect = .zero
  @State private var homeScene: CGRect = .zero

  var body: some View {
    HStack(spacing: 0) {
      HomeSidebar(showDemos: $showDemos, onNew: { showNew = true }, onOpen: openPanel,
                  reveal: reveal, arrived: arrived, wordFrame: $homeWord, sceneFrame: $homeScene)
        .frame(width: 250)
      VStack(spacing: 0) {
        if showDemos { demos } else { recents }
        if let e = model.error {
          HStack {
            Label(e, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.red).textSelection(.enabled)
            Spacer()
            Button("閉じる") { model.error = nil }
          }
          .padding(10)
          .background(.red.opacity(0.08))
        }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
      .background(HomeBackground())
      // comes in from the right as the welcome gives way
      .offset(x: 420 * (1 - reveal))
      .opacity(reveal)
    }
    .overlay {
      if let start = welcomeStart {
        TimelineView(.animation) { ctx in
          WelcomeOverlay(t: ctx.date.timeIntervalSince(start), homeWord: homeWord, homeScene: homeScene) {
            welcomeStart = Date().addingTimeInterval(-WelcomeBeat.toHome.lowerBound)
          }
        }
      }
    }
    .coordinateSpace(.named("home"))
    .onAppear {
      guard WelcomeIntro.take(reduceMotion: reduceMotion) else { return }
      reveal = 0
      arrived = false
      welcomeStart = Date()
    }
    .task(id: welcomeStart) {
      // home comes up during the move, then takes over from the welcome
      guard let start = welcomeStart else { return }
      let elapsed = Date().timeIntervalSince(start)
      try? await Task.sleep(for: .seconds(max(0, WelcomeBeat.toHome.lowerBound - elapsed)))
      guard !Task.isCancelled else { return }
      let move = WelcomeBeat.toHome.upperBound - WelcomeBeat.toHome.lowerBound
      withAnimation(.easeInOut(duration: move)) { reveal = 1 }
      try? await Task.sleep(for: .seconds(move))
      guard !Task.isCancelled else { return }
      arrived = true
      welcomeStart = nil
    }
    .tint(TsugumiPalette.rufous)
    .onReceive(NotificationCenter.default.publisher(for: .appShowDemos)) { _ in showDemos = true }
    // The home window shows no title: the key visual and the name are in it.
    .toolbar(removing: .title)
    .toolbarBackgroundVisibility(.hidden, for: .windowToolbar)
    .ignoresSafeArea(.container, edges: .top)
    .sheet(isPresented: $showNew) { newProjectSheet }
    .onChange(of: model.requestNewProject, initial: true) { _, asked in
      if asked {
        showNew = true
        model.requestNewProject = false
      }
    }
  }

  private var recents: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack {
        Text("最近のプロジェクト").font(.headline)
        Spacer()
        if let chosen = picking {
          Toggle("すべて選択", isOn: Binding(get: { !model.recents.isEmpty && chosen.count == model.recents.count },
                                          set: { picking = $0 ? Set(model.recents.map(\.path)) : [] }))
            .toggleStyle(.checkbox)
          Button("一覧から削除") {
            model.forgetRecents(chosen)
            picking = nil
          }
          .disabled(chosen.isEmpty)
          .help("一覧から外すだけで、フォルダは削除しません")
          Button("完了") { picking = nil }
        } else if !model.recents.isEmpty {
          Button("編集") { picking = [] }.help("一覧から外すプロジェクトを選ぶ")
        }
      }
      .padding(.top, 44)
      if model.recents.isEmpty {
        ContentUnavailableView("最近開いたプロジェクトはありません", systemImage: "clock",
                               description: Text("新規作成するか、既存のフォルダを開いてください。"))
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else {
        // Only the projects themselves (no empty striped rows); scrolls when long.
        SwiftUI.TimelineView(.periodic(from: .now, by: 60)) { t in
          ScrollView {
            LazyVStack(spacing: 2) {
              ForEach(model.recents) { p in recentRow(p, now: t.date) }
            }
            .padding(4)
            .background(.background.opacity(0.7), in: RoundedRectangle(cornerRadius: 10))
          }
          .scrollBounceBehavior(.basedOnSize)
        }
      }
    }
    .padding(.horizontal, 24).padding(.bottom, 20)
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
  }

  private func recentRow(_ p: RecentProject, now: Date) -> some View {
    Button {
      if var chosen = picking {
        if chosen.contains(p.path) { chosen.remove(p.path) } else { chosen.insert(p.path) }
        picking = chosen
      } else {
        onOpen(URL(fileURLWithPath: p.path))
      }
    } label: {
      HStack {
        if let chosen = picking {
          Image(systemName: chosen.contains(p.path) ? "checkmark.square.fill" : "square")
            .foregroundStyle(chosen.contains(p.path) ? Color.accentColor : .secondary)
            .accessibilityLabel(chosen.contains(p.path) ? "選択済み" : "未選択")
        }
        Image(systemName: "square.stack.3d.up").foregroundStyle(.secondary)
        VStack(alignment: .leading) {
          Text(p.name).font(.headline).lineLimit(1)
          Text(p.path).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
        }
        Spacer()
        Text(relativeAge(p.openedAt, now: now)).font(.caption).foregroundStyle(.secondary)
      }
      .padding(.horizontal, 10).padding(.vertical, 7)
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .contextMenu {
      Button("Finder で表示") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: p.path)]) }
      Button("一覧から削除") { model.forgetRecents([p.path]) }
    }
  }

  private var shownDemos: [DemoEntry] {
    model.demos.filter { d in
      (tag == nil || d.tags.contains(tag!)) &&
        (search.isEmpty || d.title.localizedCaseInsensitiveContains(search) || d.description.localizedCaseInsensitiveContains(search)
          || d.tags.contains { $0.localizedCaseInsensitiveContains(search) })
    }
  }

  private var demos: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack {
        Text("デモ").font(.title2.bold())
        Spacer()
        TextField("探す", text: $search).textFieldStyle(.roundedBorder).frame(maxWidth: 220)
      }
      .padding(.top, 44)
      Text("デモは Documents/Tsugumi に編集可能なコピーとして開きます。同梱の元データは変更されません。")
        .font(.callout).foregroundStyle(.secondary)
      // Tags to narrow the list as demos grow.
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 6) {
          TagChip(label: String(localized: "すべて"), selected: tag == nil) { tag = nil }
          ForEach(Array(Set(model.demos.flatMap(\.tags))).sorted(), id: \.self) { t in
            TagChip(label: t, selected: tag == t) { tag = tag == t ? nil : t }
          }
        }
      }
      ScrollView {
        if shownDemos.isEmpty {
          ContentUnavailableView(model.demos.isEmpty ? "デモが見つかりません" : "条件に合うデモはありません", systemImage: "questionmark.folder")
            .padding(.top, 40)
        }
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 260), spacing: 14)], spacing: 14) {
          ForEach(shownDemos) { demo in DemoCard(demo: demo) { do { onOpen(try model.copyDemo(demo)) } catch { model.error = "\(error)" } } }
        }
        .padding(.bottom, 20)
      }
    }
    .padding(.horizontal, 24)
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
  }

  private var newProjectSheet: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("新規プロジェクト").font(.title2.bold())
      TextField("名前", text: $newName)
      HStack {
        Spacer()
        Button("キャンセル") { showNew = false }
        Button("作成場所を選ぶ…") { createProject() }.disabled(newName.isEmpty).keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 380)
  }

  private func openPanel() {
    let p = NSOpenPanel()
    p.canChooseDirectories = true
    p.canChooseFiles = false
    p.message = String(localized: "tsugumi.json を含むフォルダを選んでください")
    if p.runModal() == .OK, let url = p.url { onOpen(url) }
  }

  private func createProject() {
    let p = NSSavePanel()
    p.nameFieldStringValue = newName
    p.message = String(localized: "プロジェクトのフォルダを作成する場所を選んでください")
    guard p.runModal() == .OK, let url = p.url else { return }
    do {
      try model.createProject(at: url, name: newName)
      showNew = false
      onOpen(url)
    } catch {
      model.error = "\(error)"
    }
  }
}

/// "たった今", "5 分前", "3 時間前", "2 日前": no seconds that tick.
func relativeAge(_ date: Date, now: Date = Date()) -> String {
  let s = max(0, now.timeIntervalSince(date))
  if s < 60 { return String(localized: "たった今") }
  if s < 3600 { return String(localized: "\(Int(s / 60)) 分前") }
  if s < 86400 { return String(localized: "\(Int(s / 3600)) 時間前") }
  return String(localized: "\(Int(s / 86400)) 日前")
}

/// Home background in the key visual's colours, light enough for text.
struct HomeBackground: View {
  @Environment(\.colorScheme) private var scheme

  var body: some View {
    ZStack {
      Color(nsColor: .windowBackgroundColor)
      LinearGradient(colors: [TsugumiPalette.tealTop.opacity(scheme == .dark ? 0.35 : 0.10),
                              TsugumiPalette.olive.opacity(scheme == .dark ? 0.10 : 0.08)],
                     startPoint: .topLeading, endPoint: .bottomTrailing)
    }
    .ignoresSafeArea()
  }
}

/// The left column of home: the bird on its branch with the temari, telling
/// tips, the name, and the ways in; together a little above the middle.
/// During the welcome, `reveal` brings up what it does not carry over, and
/// the name and the bird show here once they have `arrived`; the frames of
/// both are reported for the welcome to fly them to.
private struct HomeSidebar: View {
  @Binding var showDemos: Bool
  let onNew: () -> Void
  let onOpen: () -> Void
  var reveal: Double = 1
  var arrived = true
  @Binding var wordFrame: CGRect
  @Binding var sceneFrame: CGRect

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      Spacer(minLength: 0)
      HomeTipBubble()
        .padding(.horizontal, 18)
        .opacity(reveal).offset(y: 8 * (1 - reveal))
      TsugumiTemariScene(birdScale: 0.19, showsBird: arrived)
        .opacity(arrived ? 1 : max(0, reveal * 1.4 - 0.4))
        .frame(height: 150)
        .onGeometryChange(for: CGRect.self) { $0.frame(in: .named("home")) } action: { sceneFrame = $0 }
        .padding(.top, -6)
      // small here: a thicker thread than the key visual's, to read as well
      TsugumiWordmark(weight: 6.5)
        .opacity(arrived ? 1 : 0)
        .frame(width: 190, alignment: .leading)
        .onGeometryChange(for: CGRect.self) { $0.frame(in: .named("home")) } action: { wordFrame = $0 }
        .padding(.horizontal, 18)
      VStack(alignment: .leading, spacing: 8) {
        Button(action: onNew) { Label("新規プロジェクト…", systemImage: "plus.square").frame(maxWidth: .infinity, alignment: .leading) }
          .controlSize(.large).buttonStyle(.borderedProminent)
        Button(action: onOpen) { Label("既存のプロジェクトを開く…", systemImage: "folder").frame(maxWidth: .infinity, alignment: .leading) }
          .controlSize(.large)
        Button { showDemos.toggle() } label: {
          Label(showDemos ? "最近のプロジェクト" : "デモを見る", systemImage: showDemos ? "clock" : "sparkles")
        }
        .buttonStyle(.link).foregroundStyle(TsugumiPalette.cream).padding(.top, 4)
      }
      .padding(.horizontal, 18)
      .opacity(reveal).offset(y: 10 * (1 - reveal))
      Spacer(minLength: 0)
      Spacer(minLength: 0)   // twice: the block sits a little above the middle
    }
    .frame(maxHeight: .infinity)
    .background(LowPolyBackground().ignoresSafeArea())
    .environment(\.colorScheme, .dark)
  }
}

private struct TagChip: View {
  let label: String
  let selected: Bool
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      Text(label).font(.caption)
        .padding(.horizontal, 9).padding(.vertical, 4)
        .background(Capsule().fill(selected ? TsugumiPalette.rufous.opacity(0.85) : Color.secondary.opacity(0.15)))
        .foregroundStyle(selected ? Color.white : Color.primary)
    }
    .buttonStyle(.plain)
    .accessibilityAddTraits(selected ? .isSelected : [])
  }
}

/// A demo with a thumbnail drawn from its own Workflow (Cards and wires).
private struct DemoCard: View {
  let demo: DemoEntry
  let open: () -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      DemoThumbnail(url: demo.url)
        .frame(height: 130)
        .clipShape(RoundedRectangle(cornerRadius: 8))
      Text(demo.title).font(.headline)
      Text(demo.description).font(.callout).foregroundStyle(.secondary).lineLimit(3).fixedSize(horizontal: false, vertical: true)
      HStack(spacing: 4) {
        ForEach(demo.tags, id: \.self) { t in
          Text(t).font(.caption2).padding(.horizontal, 6).padding(.vertical, 2)
            .background(Capsule().fill(TsugumiPalette.olive.opacity(0.25)))
        }
      }
      HStack {
        Spacer()
        Button("コピーを開く", action: open).buttonStyle(.borderedProminent)
      }
    }
    .padding(12)
    .background(.background.opacity(0.85), in: RoundedRectangle(cornerRadius: 12))
    .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(.secondary.opacity(0.2)))
  }
}

/// The top-level Workflow of a demo in miniature: Cards as tiles coloured by
/// kind, routing Cards as small marks, wires as curves.
struct DemoThumbnail: View {
  let url: URL

  private struct Mini { var cards: [(id: String, rect: CGRect, color: Color)]; var wires: [(CGPoint, CGPoint)] }

  private var mini: Mini {
    guard let p = try? JSONValue.parse(String(contentsOf: url.appendingPathComponent("tsugumi.json"), encoding: .utf8)),
          let l = try? JSONValue.parse(String(contentsOf: url.appendingPathComponent("layout.json"), encoding: .utf8)) else {
      return Mini(cards: [], wires: [])
    }
    var cards: [(String, CGRect, Color)] = []
    for c in p["canvas"]?["cards"]?.array ?? [] {
      guard let id = c["id"]?.string else { continue }
      let pos = CGPoint(x: l["cards"]?[id]?["x"]?.double ?? 0, y: l["cards"]?[id]?["y"]?.double ?? 0)
      // The Card's own definition (definitions/<id>/): a template, a link (Card in Card) or code.
      let dir = url.appendingPathComponent("definitions/\(id)")
      let link = FileManager.default.fileExists(atPath: dir.appendingPathComponent("_link.json").path)
      let def = (try? JSONValue.parse(String(contentsOf: dir.appendingPathComponent("_card.json"), encoding: .utf8)))?["template"]?.string ?? ""
      let routing = ["fork", "branch", "merge"].contains(def)
      let color: Color = link ? TsugumiPalette.olive : routing ? TsugumiPalette.mist : def == "trigger" || def == "input" ? TsugumiPalette.rufous : TsugumiPalette.cream
      cards.append((id, CGRect(x: pos.x, y: pos.y, width: routing ? 70 : 220, height: routing ? 44 : 64), color))
    }
    var wires: [(CGPoint, CGPoint)] = []
    for w in p["canvas"]?["connections"]?.array ?? [] {
      guard let a = cards.first(where: { $0.0 == w["from"]?["card"]?.string })?.1,
            let b = cards.first(where: { $0.0 == w["to"]?["card"]?.string })?.1 else { continue }
      wires.append((CGPoint(x: a.midX, y: a.maxY), CGPoint(x: b.midX, y: b.minY)))
    }
    return Mini(cards: cards, wires: wires)
  }

  var body: some View {
    let m = mini
    Canvas { g, size in
      g.fill(Path(CGRect(origin: .zero, size: size)),
             with: .linearGradient(Gradient(colors: [TsugumiPalette.tealTop, TsugumiPalette.tealBottom]), startPoint: .zero,
                                   endPoint: CGPoint(x: size.width, y: size.height)))
      let box = m.cards.reduce(CGRect.null) { $0.union($1.rect) }
      guard !box.isNull else { return }
      let r = box.insetBy(dx: -30, dy: -30)
      let z = min(size.width / r.width, size.height / r.height)
      func t(_ p: CGPoint) -> CGPoint { CGPoint(x: (p.x - r.midX) * z + size.width / 2, y: (p.y - r.midY) * z + size.height / 2) }
      for (a, b) in m.wires {
        var path = Path()
        let s = t(a), e = t(b), dy = max(8, abs(e.y - s.y) / 2)
        path.move(to: s)
        path.addCurve(to: e, control1: CGPoint(x: s.x, y: s.y + dy), control2: CGPoint(x: e.x, y: e.y - dy))
        g.stroke(path, with: .color(TsugumiPalette.cream.opacity(0.55)), lineWidth: 1.2)
      }
      for c in m.cards {
        let o = t(c.rect.origin)
        let rect = CGRect(x: o.x, y: o.y, width: c.rect.width * z, height: c.rect.height * z)
        g.fill(Path(roundedRect: rect, cornerRadius: max(2, 8 * z)), with: .color(c.color.opacity(0.9)))
      }
    }
    .accessibilityHidden(true)
  }
}
