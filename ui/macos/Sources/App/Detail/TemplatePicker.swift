import AppKit
import SwiftUI
import TsugumiRPC

struct NewCardItems: View {
  let add: (DefinitionTemplate) -> Void
  let pickTemplate: () -> Void

  var body: some View {
    ForEach(DefinitionTemplate.allCases) { kind in
      Button { add(kind) } label: { Label(kind.label, systemImage: kind.symbol) }
    }
    Divider()
    Button("テンプレートから…", action: pickTemplate)
  }
}

struct TemplatePlacement: Identifiable {
  let id = UUID()
  let scope: [String]
  let point: CGPoint
}

struct TemplatePicker: View {
  @Environment(\.dismiss) private var dismiss
  let session: ProjectSession
  let placement: TemplatePlacement

  private enum Mode { case list, folder, git }
  @State private var entries: [Tsugumi_Host_TemplateEntryT]?
  @State private var mode: Mode = .list
  @State private var store: Tsugumi_Host_TemplateStore?
  @State private var name = ""
  @State private var folder: URL?
  @State private var url = ""
  @State private var ref = ""
  @State private var subdirectory = ""
  @State private var busy = false
  @State private var failure: String?

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      switch mode {
      case .list: list
      case .folder, .git: addition
      }
      if busy { ProgressView().controlSize(.small) }
      if let failure {
        Text(failure).foregroundStyle(.red).textSelection(.enabled)
      }
    }
    .padding(20)
    .frame(width: 460)
    .interactiveDismissDisabled(busy)
    .task { await load() }
  }

  private var list: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("テンプレートから…").font(.headline)
      if let entries {
        ScrollView {
          VStack(alignment: .leading, spacing: 12) {
            section("プロジェクト", store: .project, entries: entries)
            section("ホスト", store: .host, entries: entries)
          }
          .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(height: 235)
      } else if failure == nil {
        ProgressView()
      }
      HStack {
        Button("テンプレートを追加…") { chooseSource(.folder) }.disabled(busy || !session.isEditable)
        Button("Git からインポート…") { chooseSource(.git) }.disabled(busy || !session.isEditable)
        Spacer()
        Button("閉じる") { dismiss() }.keyboardShortcut(.cancelAction).disabled(busy)
      }
    }
  }

  private func section(_ title: LocalizedStringKey, store: Tsugumi_Host_TemplateStore,
                       entries: [Tsugumi_Host_TemplateEntryT]) -> some View {
    let choices = entries.filter { $0.store == store }
    return VStack(alignment: .leading, spacing: 4) {
      Text(title).font(.subheadline.weight(.semibold))
      if choices.isEmpty {
        Text("テンプレートがありません").foregroundStyle(.secondary)
      }
      ForEach(choices, id: \.name) { entry in
        Button {
          Task { await copy(entry) }
        } label: {
          Text(entry.name ?? "").frame(maxWidth: .infinity, alignment: .leading)
        }
        .disabled(busy || !session.isEditable)
      }
    }
  }

  private var addition: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text(mode == .git ? "Git からインポート…" : "テンプレートを追加…").font(.headline)
      Picker("保存先", selection: $store) {
        Text("選択").tag(nil as Tsugumi_Host_TemplateStore?)
        Text("プロジェクト").tag(Optional(Tsugumi_Host_TemplateStore.project))
        Text("ホスト").tag(Optional(Tsugumi_Host_TemplateStore.host))
      }
      TextField("名前", text: $name)
      if mode == .git {
        TextField("公開 Git リポジトリの HTTPS URL", text: $url)
        TextField("Git のブランチ、タグ、コミット", text: $ref)
        TextField("テンプレートのサブディレクトリ", text: $subdirectory)
      } else {
        HStack {
          Button("フォルダを選択…") { chooseFolder() }
          if let folder { Text(folder.path).foregroundStyle(.secondary).textSelection(.enabled) }
        }
      }
      HStack {
        Spacer()
        Button("戻る") {
          failure = nil
          mode = .list
        }
        .keyboardShortcut(.cancelAction)
        Button(mode == .git ? "インポート" : "追加") { Task { await save() } }
          .keyboardShortcut(.defaultAction)
          .disabled(!canSave)
      }
    }
    .disabled(busy)
  }

  private var canSave: Bool {
    guard session.isEditable, store != nil, !clean(name).isEmpty else { return false }
    return mode == .folder ? folder != nil : ![url, ref, subdirectory].contains { clean($0).isEmpty }
  }

  private func clean(_ value: String) -> String {
    value.trimmingCharacters(in: .whitespacesAndNewlines)
  }

  private func chooseSource(_ next: Mode) {
    store = nil
    name = ""
    failure = nil
    mode = next
  }

  private func chooseFolder() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    guard panel.runModal() == .OK, let selected = panel.url else { return }
    folder = selected
    if name.isEmpty { name = selected.lastPathComponent }
  }

  private func load() async {
    do { entries = try await session.listTemplates() }
    catch { failure = error.localizedDescription }
  }

  private func copy(_ entry: Tsugumi_Host_TemplateEntryT) async {
    guard !busy else { return }
    busy = true
    failure = nil
    defer { busy = false }
    do {
      try await session.copyTemplate(entry, in: placement.scope, at: placement.point)
      dismiss()
    } catch { failure = error.localizedDescription }
  }

  private func save() async {
    guard !busy, canSave, let store else { return }
    busy = true
    failure = nil
    defer { busy = false }
    let addition = Tsugumi_Host_TemplateAddT()
    addition.name = clean(name)
    if mode == .git {
      let source = Tsugumi_Host_TemplateGitT()
      source.url = clean(url)
      source.ref = clean(ref)
      source.subdirectory = clean(subdirectory)
      addition.source = Tsugumi_Host_TemplateSourceUnion(source, type: .templategit)
    } else if let folder {
      let source = Tsugumi_Host_TemplateFolderT()
      source.path = folder.path
      addition.source = Tsugumi_Host_TemplateSourceUnion(source, type: .templatefolder)
    }
    do {
      try await session.addTemplate(addition, to: store)
      mode = .list
      await load()
    } catch { failure = error.localizedDescription }
  }
}
