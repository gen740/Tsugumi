import SwiftUI

/// Asks whether the Agent may give Cards what its save needs (network,
/// running without the sandbox, folders of the Mac). Each kind can be allowed
/// without asking from now on, as in Settings > API.
struct PermissionPromptView: View {
  let prompt: ChatController.PermissionPrompt
  let answer: (_ allow: Bool, _ always: Set<CardPermission>) -> Void
  @State private var always: Set<CardPermission> = []

  private var kinds: [CardPermission] { CardPermission.allCases.filter { k in prompt.requests.contains { $0.kind == k } } }

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Label("Agent が Card の権限を広げようとしています", systemImage: "hand.raised")
        .font(.headline)
      Text("許可すると、この保存が行われます。許可しなければ何も保存されず、Agent には理由を説明するよう伝えます。")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      VStack(alignment: .leading, spacing: 6) {
        ForEach(prompt.requests) { r in
          HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: r.kind.symbol).frame(width: 18)
            VStack(alignment: .leading, spacing: 1) {
              Text("\(r.card): \(r.kind.label)")
              if let f = r.folder { Text(f).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled) }
            }
          }
        }
      }
      .padding(10)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 8))
      VStack(alignment: .leading, spacing: 4) {
        Text("今後は確認しない（設定の「API」でいつでも変えられます）").font(.caption).foregroundStyle(.secondary)
        ForEach(kinds) { k in
          Toggle(k.label, isOn: Binding(get: { always.contains(k) }, set: { on in if on { always.insert(k) } else { always.remove(k) } }))
            .toggleStyle(.checkbox)
        }
      }
      HStack {
        Spacer()
        Button("許可しない") { answer(false, []) }.keyboardShortcut(.cancelAction)
        Button("許可する") { answer(true, always) }.keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 440)
    .interactiveDismissDisabled()
  }
}
