import SwiftUI

/// Core supplies the same structured description for live and restored requests.
enum ProjectAccessLabel {
  static func neededNow(_ request: String) -> Bool {
    guard let data = String(request.dropFirst("project_access ".count)).data(using: .utf8),
          let v = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return true }
    return v["needed_now"] as? Bool ?? true
  }
  static func text(_ request: String) -> String {
    let prefix = "project_access helper_scope\n"
    if request.hasPrefix(prefix), let data = String(request.dropFirst(prefix.count)).data(using: .utf8),
       let v = try? JSONSerialization.jsonObject(with: data) as? [String: Any], let task = v["task"] as? [String: Any] {
      return "$" + (task["name"] as? String ?? "") + "\n" + (task["skill_path"] as? String ?? "") + "\n" + ["read", "write", "run"].map { access in
        access + ": " + ((task[access + "_cards"] as? [String] ?? []).joined(separator: ", "))
      }.joined(separator: "\n")
    }
    return request
  }
}

private func accessOperation(_ name: String) -> String {
  switch name {
  case "Read": return String(localized: "内容の読み取り")
  case "Write": return String(localized: "内容の変更")
  case "Run": return String(localized: "Card の実行")
  case "CardPermissions": return String(localized: "Card の権限の変更")
  case "Shell": return String(localized: "シェルコマンドの実行")
  default: return name
  }
}

/// Shows core-owned access requests and approvals for a single operation.
struct PermissionPromptView: View {
  let prompt: ChatController.PermissionPrompt
  let answer: (_ allow: Bool, _ always: Set<CardPermission>, _ selected: Set<String>) -> Void
  var reload = false
  @State private var always: Set<CardPermission> = []
  @State private var selected: Set<String>

  init(prompt: ChatController.PermissionPrompt,
       answer: @escaping (Bool, Set<CardPermission>, Set<String>) -> Void, reload: Bool = false) {
    self.prompt = prompt
    self.answer = answer
    self.reload = reload
    _selected = State(initialValue: prompt.defaultSelection)
  }

  private var kinds: [CardPermission] { (prompt.operation != nil || reload) ? [] : CardPermission.allCases.filter { k in prompt.requests.contains { $0.kind == k } } }

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Label(reload ? "外部の変更が Card の権限を広げます" : prompt.operation != nil ? "Agent がこの操作の承認を求めています" : prompt.access.isEmpty ? "Agent が Card の権限を広げようとしています" : "Agent がアクセスの許可を求めています", systemImage: "hand.raised")
        .font(.headline)
      if prompt.groups.isEmpty { Text(reload ? "表示された権限を許可して、外部の変更を読み込みますか？" : prompt.operation != nil ? "表示された対象への、この操作だけを承認します。次の操作には再び承認が必要です。拒否すると何も変更しません。" : prompt.access.isEmpty ? "許可すると、この保存が行われます。許可しなければ何も保存されず、Agent には理由を説明するよう伝えます。" : "表示された対象へのアクセスを許可します。読み取った内容は接続先のモデルに送信される場合があります。")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
      VStack(alignment: .leading, spacing: 6) {
        if let operation = prompt.operation {
          ScrollView {
            Text(operation).font(.caption.monospaced()).textSelection(.enabled)
              .frame(maxWidth: .infinity, alignment: .leading)
          }.frame(maxHeight: 360)
        }
        ForEach(Array(prompt.groups.enumerated()), id: \.offset) { _, group in
          VStack(alignment: .leading, spacing: 6) {
            Text(group.title ?? "").font(.callout.weight(.semibold))
            HStack(alignment: .firstTextBaseline) {
              Text(group.displayPath ?? "").help(group.path ?? "").textSelection(.enabled)
              Text("プロジェクト全体・取り消すまで").fontWeight(.bold)
            }.font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            ForEach(Array(group.lines.compactMap { $0 }.enumerated()), id: \.offset) { _, line in
              let request = line.request ?? ""
              Toggle(isOn: Binding(get: { selected.contains(request) }, set: { on in
                if on { selected.insert(request) } else { selected.remove(request) }
              })) {
                HStack(alignment: .firstTextBaseline) {
                  Text(accessOperation(line.operation ?? "")).fixedSize(horizontal: false, vertical: true)
                  if line.neededNow { Text("今の操作に必要").font(.caption).foregroundStyle(.secondary) }
                }
              }.toggleStyle(.checkbox)
            }
          }
        }
        ForEach(prompt.operation == nil ? prompt.access.filter { request in
          !prompt.groups.contains { group in group.lines.contains { $0?.request == request } }
        } : [], id: \.self) { request in
          Toggle(isOn: Binding(get: { selected.contains(request) }, set: { on in
            if on { selected.insert(request) } else { selected.remove(request) }
          })) { Text(ProjectAccessLabel.text(request)).fixedSize(horizontal: false, vertical: true).textSelection(.enabled) }.toggleStyle(.checkbox)
        }
        ForEach(prompt.requests) { r in
          Toggle(isOn: Binding(get: { selected.contains(r.raw) }, set: { on in
            if on { selected.insert(r.raw) } else { selected.remove(r.raw) }
          })) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
              Image(systemName: r.kind.symbol).frame(width: 18)
              VStack(alignment: .leading, spacing: 1) {
                Text("\(r.card): \(r.kind.label)")
                if let f = r.folder { Text(f).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled) }
              }
            }
          }.toggleStyle(.checkbox)
        }
      }
      .padding(10)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 8))
      if !prompt.groups.isEmpty {
        Text("読み取った内容は接続先のモデルに送信される場合があります。").font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      }
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
        Button("許可しない") { answer(false, [], []) }.keyboardShortcut(.cancelAction)
        Button("許可する") { answer(true, always, selected) }.keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 440)
    .interactiveDismissDisabled()
    .task { await PermissionCatalog.shared.load() }
  }
}
