import SwiftUI

/// Shows core-owned access requests and approvals for a single operation.
struct PermissionPromptView: View {
  let prompt: ChatController.PermissionPrompt
  let answer: (_ allow: Bool, _ always: Set<CardPermission>) -> Void
  var reload = false
  @State private var always: Set<CardPermission> = []

  private var kinds: [CardPermission] { (prompt.operation != nil || reload) ? [] : CardPermission.allCases.filter { k in prompt.requests.contains { $0.kind == k } } }

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Label(reload ? "外部の変更が Card の権限を広げます" : prompt.operation != nil ? "Agent がこの操作の承認を求めています" : prompt.access.isEmpty ? "Agent が Card の権限を広げようとしています" : "Agent がアクセスの許可を求めています", systemImage: "hand.raised")
        .font(.headline)
      Text(reload ? "表示された権限を許可して、外部の変更を読み込みますか？" : prompt.operation != nil ? "表示された対象への、この操作だけを承認します。次の操作には再び承認が必要です。拒否すると何も変更しません。" : prompt.access.isEmpty ? "許可すると、この保存が行われます。許可しなければ何も保存されず、Agent には理由を説明するよう伝えます。" : "表示された対象へのアクセスを許可します。読み取った内容は接続先のモデルに送信される場合があります。")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      VStack(alignment: .leading, spacing: 6) {
        if let operation = prompt.operation {
          ScrollView {
            Text(operation).font(.caption.monospaced()).textSelection(.enabled)
              .frame(maxWidth: .infinity, alignment: .leading)
          }.frame(maxHeight: 360)
        }
        ForEach(prompt.operation == nil ? prompt.access : [], id: \.self) { request in
          Text(String(request.dropFirst("project_access ".count))).textSelection(.enabled)
        }
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
        if !kinds.isEmpty {
          Text("今後は確認しない（設定の「API」でいつでも変えられます）").font(.caption).foregroundStyle(.secondary)
        }
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
