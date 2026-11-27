import SwiftUI

/// An operation approval as hostd describes it: the tool, its targets as the
/// project reports them, and the arguments the Agent proposed.
struct OperationApproval {
  let name: String
  let targets: [String: Any]
  let args: [String: Any]
  /// Every line hostd sent, JSON laid out to read.
  let detail: String

  private static let mark = "project_access operation_approval\n"
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
    let name = Self.text(args["name"]).isEmpty ? Self.text(targets["name"]) : Self.text(args["name"])
    switch self.name {
    case "nix_path_lock_update":
      return [name.isEmpty ? String(localized: "すべての Nix のソースの固定（tsugumi.lock）を更新") : String(localized: "Nix のソース \(name) の固定（tsugumi.lock）を更新")]
    case "run_history_delete":
      let count = Self.text(targets["run_count"]).isEmpty ? "0" : Self.text(targets["run_count"])
      return [String(localized: "実行記録 \(count) 件と、その値・ログ・残したファイルを削除")]
    case "run_scratch_delete":
      let count = String((targets["folders"] as? [Any])?.count ?? 0)
      return [String(localized: "作業フォルダ \(count) 個を削除（実行記録と残した出力は残ります）")]
    case "project_cache_clear":
      return [String(localized: "このプロジェクトの Nix ビルドキャッシュを削除（実行データは残ります）")]
    case "template_add":
      let path = Self.text(args["path"])
      return [String(localized: "フォルダ \(path) をテンプレート \(name) として追加")]
    case "template_import":
      let url = Self.text(args["url"])
      return [String(localized: "\(url) からテンプレート \(name) を取り込む")]
    case "version_checkout":
      let id = String(Self.text((targets["version"] as? [String: Any])?["id"]).prefix(12))
      let version = id.isEmpty ? Self.text(args["commit"]) : id
      return [String(localized: "今の状態を記録してから、版 \(version) に切り替える")]
    case "version_init":
      return [String(localized: "版の記録を始める")]
    case "version_mark":
      return [String(localized: "今の状態を版 \(name) として残す")]
    default:
      return [self.name]
    }
  }
}

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
      Label(reload ? "外部の変更が Card の権限を広げます" : prompt.operation != nil ? "Agent がこの操作の承認を求めています" : prompt.access.isEmpty ? "Agent が Card の権限を広げようとしています" : "Agent がアクセスの許可を求めています", systemImage: "hand.raised")
        .font(.headline)
      if prompt.groups.isEmpty { Text(reload ? "表示された権限を許可して、外部の変更を読み込みますか？" : prompt.operation != nil ? "表示された対象への、この操作だけを承認します。次の操作には再び承認が必要です。拒否すると何も変更しません。" : prompt.access.isEmpty ? "許可すると、この保存が行われます。許可しなければ何も保存されず、Agent には理由を説明するよう伝えます。" : "表示された対象へのアクセスを許可します。読み取った内容は接続先のモデルに送信される場合があります。")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
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
