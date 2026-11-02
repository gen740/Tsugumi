import AppKit
import SwiftUI
import TsugumiRPC

/// Home (new, existing and recent projects) and Demos (opened as editable copies).
struct ProjectChooser: View {
  @Environment(AppModel.self) private var model
  let onOpen: (URL) -> Void
  @State private var page = Page.home
  @State private var newName = ""
  @State private var showNew = false

  enum Page: String, CaseIterable, Identifiable {
    case home, demos
    var id: String { rawValue }
    var title: String { self == .home ? "ホーム" : "デモ" }
  }

  var body: some View {
    VStack(spacing: 0) {
      Picker("", selection: $page) {
        ForEach(Page.allCases) { Text($0.title).tag($0) }
      }
      .pickerStyle(.segmented).labelsHidden().frame(width: 240).padding(12)
      Divider()
      switch page {
      case .home: home
      case .demos: demos
      }
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
    .sheet(isPresented: $showNew) { newProjectSheet }
  }

  private var home: some View {
    HStack(alignment: .top, spacing: 24) {
      VStack(alignment: .leading, spacing: 12) {
        Text("Tsugumi").font(.largeTitle.bold())
        Text("Card をつないで処理の流れを作り、隔離環境で実行します。").foregroundStyle(.secondary)
        Button { showNew = true } label: { Label("新規プロジェクト…", systemImage: "plus.square") }
          .controlSize(.large).buttonStyle(.borderedProminent)
        Button { openPanel() } label: { Label("既存のプロジェクトを開く…", systemImage: "folder") }
          .controlSize(.large)
        Button { page = .demos } label: { Label("デモを見る", systemImage: "sparkles") }
          .controlSize(.large)
      }
      .frame(width: 280, alignment: .leading)
      VStack(alignment: .leading) {
        Text("最近のプロジェクト").font(.headline)
        if model.recents.isEmpty {
          ContentUnavailableView("最近開いたプロジェクトはありません", systemImage: "clock",
                                 description: Text("新規作成するか、既存のフォルダを開いてください。"))
        } else {
          List(model.recents) { p in
            Button { onOpen(URL(fileURLWithPath: p.path)) } label: {
              HStack {
                Image(systemName: "square.stack.3d.up").foregroundStyle(.secondary)
                VStack(alignment: .leading) {
                  Text(p.name).font(.headline)
                  Text(p.path).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                }
                Spacer()
                Text(p.openedAt, style: .relative).font(.caption).foregroundStyle(.secondary)
              }
              .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .contextMenu {
              Button("Finder で表示") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: p.path)]) }
            }
          }
          .listStyle(.inset(alternatesRowBackgrounds: true))
        }
      }
    }
    .padding(24)
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
  }

  private var demos: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 14) {
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
      .padding(24)
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
