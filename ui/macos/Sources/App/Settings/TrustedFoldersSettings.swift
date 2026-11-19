import AppKit
import SwiftUI

/// Host-owned trust shared by every UI and Agent conversation on this machine.
struct TrustedFoldersSettings: View {
  @HostStorage("agent.trustedFolders") private var folders: [String] = []
  @State private var settings = CorePreferences.shared

  var body: some View {
    Section {
      ForEach(folders, id: \.self) { folder in
        HStack {
          Text(folder).textSelection(.enabled)
          Spacer()
          Button("削除") { folders.removeAll { $0 == folder } }
        }
      }
      Button("フォルダを追加…") {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        if panel.runModal() == .OK, let url = panel.url, !folders.contains(url.path) {
          folders.append(url.path)
        }
      }
      if let error = settings.error { Text(error).foregroundStyle(.red) }
    } header: {
      InfoHeader("信頼するフォルダ", info: "このフォルダ内のプロジェクトを、下の階層も含めて信頼します。シンボリックリンクの参照先が外にあるプロジェクトは含みません。")
    }
    .task(id: folders) {
      await settings.reload()
    }
  }
}
