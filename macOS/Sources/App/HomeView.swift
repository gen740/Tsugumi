import AppKit
import SwiftUI

struct HomeView: View {
  @Environment(AppModel.self) private var model
  @Environment(\.openWindow) private var openWindow
  @State private var newName = ""
  @State private var showNew = false

  var body: some View {
    NavigationSplitView {
      List {
        Section("Projects") {
          ForEach(model.recents) { p in
            Button { open(URL(fileURLWithPath: p.path)) } label: {
              VStack(alignment: .leading) {
                Text(p.name).font(.headline)
                Text(p.path).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
              }
            }
            .buttonStyle(.plain)
          }
          if model.recents.isEmpty { Text("No projects yet").foregroundStyle(.secondary) }
        }
      }
      .navigationSplitViewColumnWidth(min: 240, ideal: 280)
      .toolbar {
        Button("New Project…") { showNew = true }
        Button("Open…") { openPanel() }
      }
    } detail: {
      ScrollView {
        VStack(alignment: .leading, spacing: 16) {
          Text("Demos").font(.largeTitle.bold())
          Text("Opening a demo creates an editable copy in Documents/Tsugumi; the bundled original is never changed.")
            .foregroundStyle(.secondary)
          ForEach(model.demos) { demo in
            GroupBox {
              HStack(alignment: .top) {
                VStack(alignment: .leading, spacing: 6) {
                  Text(demo.title).font(.title3.bold())
                  Text(demo.description).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                Spacer()
                Button("Open a Copy") {
                  do { open(try model.copyDemo(demo)) } catch { model.error = "\(error)" }
                }
                .buttonStyle(.borderedProminent)
              }
              .padding(6)
            }
          }
          if let e = model.error { Text(e).foregroundStyle(.red) }
        }
        .padding(24)
      }
    }
    .sheet(isPresented: $showNew) {
      VStack(alignment: .leading, spacing: 12) {
        Text("New Project").font(.title2.bold())
        TextField("Name", text: $newName)
        HStack {
          Spacer()
          Button("Cancel") { showNew = false }
          Button("Create…") { createProject() }.disabled(newName.isEmpty).keyboardShortcut(.defaultAction)
        }
      }
      .padding(20)
      .frame(width: 380)
    }
  }

  private func open(_ url: URL) {
    do {
      _ = try model.session(for: url)
      openWindow(id: "project", value: url)
    } catch {
      model.error = "\(error)"
    }
  }

  private func openPanel() {
    let p = NSOpenPanel()
    p.canChooseDirectories = true
    p.canChooseFiles = false
    p.message = "Choose a folder containing tsugumi.json"
    if p.runModal() == .OK, let url = p.url { open(url) }
  }

  private func createProject() {
    let p = NSSavePanel()
    p.nameFieldStringValue = newName
    p.message = "Choose where to create the project folder"
    guard p.runModal() == .OK, let url = p.url else { return }
    do {
      try model.createProject(at: url, name: newName)
      showNew = false
      open(url)
    } catch {
      model.error = "\(error)"
    }
  }
}
