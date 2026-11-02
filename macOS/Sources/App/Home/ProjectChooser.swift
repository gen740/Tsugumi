import AppKit
import SwiftUI
import TsugumiRPC

/// Home (new, existing and recent projects) and the demo list, entered
/// from "デモを見る" with a way back.
struct ProjectChooser: View {
  @Environment(AppModel.self) private var model
  let onOpen: (URL) -> Void
  @State private var showDemos = false
  @State private var newName = ""
  @State private var showNew = false

  var body: some View {
    VStack(spacing: 0) {
      if showDemos { demos } else { home }
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
    .background(HomeBackground())
    .tint(TsugumiPalette.rufous)
    // The home window shows no title: the key visual and the name are in it.
    .toolbar(removing: .title)
    .toolbarBackgroundVisibility(.hidden, for: .windowToolbar)
    .sheet(isPresented: $showNew) { newProjectSheet }
  }

  private var home: some View {
    HStack(alignment: .top, spacing: 28) {
      VStack(alignment: .leading, spacing: 12) {
        Image(nsImage: NSApp.applicationIconImage).resizable().interpolation(.high)
          .frame(width: 44, height: 44)
          .accessibilityHidden(true)
        Text("Tsugumi").font(.largeTitle.bold())
        Button { showNew = true } label: { Label("新規プロジェクト…", systemImage: "plus.square") }
          .controlSize(.large).buttonStyle(.borderedProminent)
        Button { openPanel() } label: { Label("既存のプロジェクトを開く…", systemImage: "folder") }
          .controlSize(.large)
        Button { showDemos = true } label: { Label("デモを見る", systemImage: "sparkles") }
          .buttonStyle(.link)
          .padding(.top, 6)
      }
      .frame(width: 260, alignment: .leading)
      VStack(alignment: .leading, spacing: 8) {
        Text("最近のプロジェクト").font(.headline)
        if model.recents.isEmpty {
          ContentUnavailableView("最近開いたプロジェクトはありません", systemImage: "clock",
                                 description: Text("新規作成するか、既存のフォルダを開いてください。"))
            .frame(maxHeight: 220)
        } else {
          // Only the projects themselves: no empty striped rows below them.
          SwiftUI.TimelineView(.periodic(from: .now, by: 60)) { t in
            ScrollView {
              VStack(spacing: 2) {
                ForEach(model.recents) { p in recentRow(p, now: t.date) }
              }
              .padding(4)
            }
            .scrollBounceBehavior(.basedOnSize)
            .background(.background.opacity(0.7), in: RoundedRectangle(cornerRadius: 10))
            .fixedSize(horizontal: false, vertical: true)
          }
        }
        Spacer(minLength: 0)
      }
    }
    .padding(28)
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
  }

  private func recentRow(_ p: RecentProject, now: Date) -> some View {
    Button { onOpen(URL(fileURLWithPath: p.path)) } label: {
      HStack {
        Image(systemName: "square.stack.3d.up").foregroundStyle(.secondary)
        VStack(alignment: .leading) {
          Text(p.name).font(.headline)
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
    }
  }

  private var demos: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 14) {
        Button { showDemos = false } label: { Label("ホーム", systemImage: "chevron.backward") }
          .buttonStyle(.link)
        Text("デモ").font(.largeTitle.bold())
        Text("デモは Documents/Tsugumi に編集可能なコピーとして開きます。同梱の元データは変更されません。")
          .foregroundStyle(.secondary)
        if model.demos.isEmpty {
          ContentUnavailableView("デモが見つかりません", systemImage: "questionmark.folder")
        }
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 300), spacing: 14)], spacing: 14) {
          ForEach(model.demos) { demo in
            GroupBox {
              VStack(alignment: .leading, spacing: 8) {
                Text(demo.title).font(.title3.bold())
                Text(demo.description).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 4)
                HStack {
                  Spacer()
                  Button("コピーを開く") {
                    do { onOpen(try model.copyDemo(demo)) } catch { model.error = "\(error)" }
                  }
                  .buttonStyle(.borderedProminent)
                }
              }
              .frame(maxWidth: .infinity, minHeight: 120, alignment: .topLeading)
              .padding(6)
            }
          }
        }
      }
      .padding(28)
    }
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
    p.message = "tsugumi.json を含むフォルダを選んでください"
    if p.runModal() == .OK, let url = p.url { onOpen(url) }
  }

  private func createProject() {
    let p = NSSavePanel()
    p.nameFieldStringValue = newName
    p.message = "プロジェクトのフォルダを作成する場所を選んでください"
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
