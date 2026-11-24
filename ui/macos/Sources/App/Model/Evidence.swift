import TsugumiAgentClient
import Foundation
import TsugumiRPC

/// The same project-local address grammar as ui/web/src/model/evidence.ts.
struct EvidenceTarget: Hashable, Identifiable {
  var id: String { href }
  let href: String
  let kind: String
  var card: [String]?
  var run: String?
  var invocation: UInt64?
  var stream = "stdout"
  var offset: UInt64 = 0
  var artifact: String?
  var member: String?
  var file: String?
  var lineStart: Int?
  var lineEnd: Int?

  static func recognizes(_ href: String) -> Bool {
    ["card:", "run:", "log:", "artifact:"].contains { href.lowercased().hasPrefix($0) }
  }
  static func parse(_ href: String) -> Self? {
    let pattern = #"^(card|run|log|artifact):([^?#]*)(?:\?([^#]*))?(?:#([^#]*))?$"#
    guard let regex = try? NSRegularExpression(pattern: pattern),
          let match = regex.firstMatch(in: href, range: NSRange(href.startIndex..., in: href)) else { return nil }
    func part(_ n: Int) -> String? { Range(match.range(at: n), in: href).map { String(href[$0]) } }
    func number(_ s: String?) -> UInt64? {
      guard let s, s.range(of: #"^(0|[1-9][0-9]*)$"#, options: .regularExpression) != nil else { return nil }
      return UInt64(s)
    }
    func validCard(_ parts: [String]) -> Bool {
      !parts.isEmpty && parts.allSatisfy { validName(String($0)) }
    }
    let kind = part(1)!, query = part(3), fragment = part(4)
    var parts = part(2)!.components(separatedBy: "/")
    var target = Self(href: href, kind: kind)
    if kind == "card" {
      guard fragment == nil, validCard(parts) else { return nil }
      target.card = parts
      guard let query else { return target }
      guard query.removingPercentEncoding != nil,
            let components = URLComponents(string: "evidence:?" + query.replacingOccurrences(of: "+", with: "%20")) else { return nil }
      let items = components.queryItems ?? []
      guard Set(items.map(\.name)).count == items.count,
            Set(items.map(\.name)) == ["file", "lines"],
            let file = items.first(where: { $0.name == "file" })?.value,
            !file.contains("\\"), !file.unicodeScalars.contains(where: { $0.value < 32 }),
            !file.components(separatedBy: "/").contains(where: { $0.isEmpty || $0 == "." || $0 == ".." }),
            let lines = items.first(where: { $0.name == "lines" })?.value?.components(separatedBy: "-"), lines.count == 2,
            let first = number(lines[0]), let last = number(lines[1]), first > 0, last >= first, last <= 9_007_199_254_740_991 else { return nil }
      target.file = file; target.lineStart = Int(first); target.lineEnd = Int(last)
      return target
    }
    let run = parts.removeFirst()
    guard run.range(of: #"^[1-9][0-9]{0,18}$"#, options: .regularExpression) != nil,
          let runNumber = Int64(run), runNumber > 0,
          parts.isEmpty || validCard(parts) else { return nil }
    target.run = run
    target.card = parts.isEmpty ? nil : parts
    if kind == "run" {
      guard query == nil else { return nil }
      if fragment != nil {
        guard !parts.isEmpty, let n = number(fragment), n > 0 else { return nil }
        target.invocation = n
      }
      return target
    }
    guard fragment == nil else { return nil }
    let encoded = (query ?? "").replacingOccurrences(of: "+", with: "%20")
    guard encoded.removingPercentEncoding != nil, let components = URLComponents(string: "evidence:?" + encoded) else { return nil }
    let items = components.queryItems ?? []
    guard Set(items.map(\.name)).count == items.count else { return nil }
    var params: [String: String] = [:]
    for item in items { params[item.name] = item.value ?? "" }
    if kind == "artifact" {
      guard parts.isEmpty, Set(params.keys).isSubset(of: ["id", "path"]), let id = params["id"], !id.isEmpty else { return nil }
      target.artifact = id
      if let path = params["path"] {
        guard !path.isEmpty, !path.components(separatedBy: "/").contains(where: { $0.isEmpty || $0 == "." || $0 == ".." }) else { return nil }
        target.member = path
      }
      return target
    }
    guard Set(params.keys).isSubset(of: ["invocation", "stream", "offset"]) else { return nil }
    if let raw = params["invocation"] {
      guard !parts.isEmpty, let n = number(raw), n > 0 else { return nil }
      target.invocation = n
    }
    if let raw = params["offset"] { guard let n = number(raw) else { return nil }; target.offset = n }
    target.stream = params["stream"] ?? "stdout"
    guard ["stdout", "stderr", "build", "journal"].contains(target.stream) else { return nil }
    return target
  }
  var logStream: Tsugumi_Data_LogStream {
    switch stream { case "stderr": return .standardError; case "build": return .build; case "journal": return .journal; default: return .standardOutput }
  }

  @MainActor func exists(in session: ProjectSession) async -> Bool {
    if kind == "card" { return session.resolution[key(card!)] != nil }
    guard let rpc = session.rpc, let run else { return false }
    do {
      _ = try await rpc.runStatus(runId: run)
      if let card {
        let r = try await rpc.record(runId: run, card: card, invocation: invocation)
        if let invocation, r.record?.summary.invocation != invocation { return false }
      }
      if kind == "log" {
        let log = try await rpc.logRange(runId: run, card: card, invocation: invocation, stream: logStream, offset: offset, limit: 4)
        return log.exists != false && offset <= (log.sizeBytes ?? 0)
      }
      if kind == "artifact" {
        return try await rpc.artifacts(runId: run).contains { a in
          a.artifactId == artifact && (member == nil || a.members.contains { $0?.path == member })
        }
      }
      return true
    } catch { return false }
  }
}

/// Source-citing paragraphs can be kept individually; ordinary answers stay in chat.
struct ChatFinding: Identifiable {
  var id: String { text }
  let text: String
  let target: EvidenceTarget
  static func list(_ text: String) -> [Self] {
    guard let regex = try? NSRegularExpression(pattern: #"\[[^\]]*\]\((card:[^\s)]+)\)"#) else { return [] }
    return text.components(separatedBy: "\n\n").compactMap { paragraph in
      for match in regex.matches(in: paragraph, range: NSRange(paragraph.startIndex..., in: paragraph)) {
        guard let range = Range(match.range(at: 1), in: paragraph), let target = EvidenceTarget.parse(String(paragraph[range])), target.file != nil else { continue }
        return Self(text: paragraph, target: target)
      }
      return nil
    }
  }
}
