import SwiftUI

/// An operation approval as hostd describes it: the tool, its targets as the
/// project reports them, and the arguments the Agent proposed.
struct OperationApproval {
  let name: String
  let targets: [String: Any]
  let args: [String: Any]
  /// Every line hostd sent, JSON laid out to read.
  let detail: String

  private static let mark = "operation_approval\n"
  private static let proposed = "Proposed arguments: "

  init?(_ asked: [String]) {
    guard let raw = asked.first(where: { $0.hasPrefix(Self.mark) }) else { return nil }
    let lines = String(raw.dropFirst(Self.mark.count)).components(separatedBy: "\n")
    func object(_ text: String?) -> [String: Any] {
      guard let data = text?.data(using: .utf8) else { return [:] }
      return (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
    }
    name = lines.first ?? ""
    targets = object(lines.count > 1 ? lines[1] : nil)
    args = object(lines.first(where: { $0.hasPrefix(Self.proposed) }).map { String($0.dropFirst(Self.proposed.count)) })
    detail = lines.map { line in
      let prefix = line.hasPrefix(Self.proposed) ? Self.proposed : ""
      guard let data = String(line.dropFirst(prefix.count)).data(using: .utf8),
            let value = try? JSONSerialization.jsonObject(with: data),
            let formatted = try? JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]),
            let text = String(data: formatted, encoding: .utf8) else { return line }
      return prefix + text
    }.joined(separator: "\n")
  }

  private static func text(_ value: Any?) -> String {
    if let s = value as? String { return s }
    if let n = value as? NSNumber { return n.stringValue }
    return ""
  }

  /// What approving will do, in sentences; the detail keeps the exact request.
  @MainActor var summary: [String] {
    if let permissions = targets["permissions"] as? [Any] {
      var out: [String] = []
      var sources: [String] = []
      for raw in permissions.map(Self.text) where !raw.isEmpty {
        guard let request = CardPermission.Request(raw) else { out.append(raw); continue }
        let folder = request.folder.map { " (\($0))" } ?? ""
        if request.kind.rawValue == "nix_source" { sources.append(request.card + folder) }
        else { out.append("\(request.kind.label): \(request.card)\(folder)") }
      }
      if !sources.isEmpty {
        let list = sources.joined(separator: ", ")
        out.insert(String(localized: "Nix のソースを追加: \(list)"), at: 0)
      }
      return out
    }
    if name == "run_scratch_delete" {
      let count = String((targets["folders"] as? [Any])?.count ?? 0)
      return [String(localized: "作業フォルダ \(count) 個を削除（実行記録と残した出力は残ります）")]
    }
    return [name]
  }
}

/// Shows a decision the core waits for: one call's operation or what a save gives Cards.
struct PermissionPromptView: View {
  let prompt: ChatController.PermissionPrompt
  let answer: (_ allow: Bool, _ selected: Set<String>) -> Void
  var reload = false
  @State private var selected: Set<String>
  @State private var showsDetail = false

  init(prompt: ChatController.PermissionPrompt,
       answer: @escaping (Bool, Set<String>) -> Void, reload: Bool = false) {
    self.prompt = prompt
    self.answer = answer
    self.reload = reload
    _selected = State(initialValue: prompt.defaultSelection)
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Label(reload ? "外部の変更が Card の権限を広げます" : prompt.operation != nil ? "Agent がこの操作の承認を求めています" : "Agent が Card の権限を広げようとしています", systemImage: "hand.raised")
        .font(.headline)
      Text(reload ? "表示された権限を許可して、外部の変更を読み込みますか？" : prompt.operation != nil ? "表示された対象への、この操作だけを承認します。次の操作には再び承認が必要です。拒否すると何も変更しません。" : "許可すると、この保存が行われます。許可しなければ何も保存されず、Agent には理由を説明するよう伝えます。")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      VStack(alignment: .leading, spacing: 6) {
        if let operation = prompt.operation {
          ForEach(Array(operation.summary.enumerated()), id: \.offset) { _, line in
            Text(verbatim: line).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
          }
          DisclosureGroup("詳細", isExpanded: $showsDetail) {
            ScrollView {
              Text(verbatim: [prompt.callId ?? "", operation.detail].filter { !$0.isEmpty }.joined(separator: "\n"))
                .font(.caption.monospaced()).textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
            }.frame(maxHeight: 300)
          }.font(.caption)
        } else if let callId = prompt.callId, !callId.isEmpty {
          Text(callId).font(.caption.monospaced()).textSelection(.enabled)
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
      HStack {
        Spacer()
        Button("許可しない") { answer(false, []) }.keyboardShortcut(.cancelAction)
        Button("許可する") { answer(true, selected) }.keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 440)
    .interactiveDismissDisabled()
  }
}
