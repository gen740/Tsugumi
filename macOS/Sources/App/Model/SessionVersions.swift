import Foundation
import TsugumiAgent
import TsugumiRPC

/// "Version summaries": a small model writes what a version changed from its diff, only
/// while switched on (the version tree asks first): code and settings go to
/// the model's provider, never data files. It costs tokens of its own and is
/// not made on the ChatGPT plan, whose allowance is for the Agent.
@MainActor
enum VersionSummarizer {
  static let defaultsKey = "versions.summaries"
  static var enabled: Bool { UserDefaults.standard.bool(forKey: defaultsKey) }

  /// The small model of the Agent's provider (none for the local fixture or the plan).
  static func config() -> ProviderConfig? {
    guard var c = ThoughtSummarizer.config(), c.auth != .chatGPT else { return nil }
    c.maxTokens = 1500  // room for its own reasoning on a large diff; the answer is cut to one line
    return c
  }

  /// What a summary is written from: the version's message and what it changed.
  nonisolated static func input(message: String, changed: [String], diff: String, truncated: Bool) -> String {
    let cards = changed.map { $0.isEmpty ? "the Workflow itself" : $0 }.joined(separator: ", ")
    return "Commit message: \(message.trimmingCharacters(in: .whitespacesAndNewlines))\nChanged Cards: \(cards)\n\n"
      + "Diff\(truncated ? " (cut short)" : ""):\n\(diff)"
  }

  /// One line, however the model answers.
  nonisolated static func clean(_ text: String) -> String {
    String(text.split(whereSeparator: \.isWhitespace).joined(separator: " ").prefix(300))
  }

  static func summarize(project: AgentProject, commit: String, config: ProviderConfig) async throws -> String {
    let client = CoreAgentClient.shared
    _ = try await client.call("RegisterProjectRequest", ["project_id": project.id, "endpoint": project.endpoint,
      "token": project.token, "title": project.title])
    try await client.configure(config)
    let result = try await client.call("VersionSummarizeRequest", ["project_id": project.id, "commit": commit])
    return clean(result["text"] as? String ?? "")
  }

}

/// The project's versions (core/src/versions.cc): a git repository whose
/// commits are what runs ran, with their results in notes and named
/// versions as tags.
extension ProjectSession {
  func refreshVersions() async {
    guard let rpc else { return }
    versionGraph = try? await rpc.versionGraph()
  }

  func loadVersionCanvas(_ commit: String) async {
    guard let rpc, versionCanvases[commit] == nil else { return }
    if let canvas = try? await rpc.versionCanvas(commit: commit) { versionCanvases[commit] = canvas }
  }

  /// Has a small model write what `commit` changed, once, while switched on:
  /// as a run of it ends and when it is hovered without a summary.
  func summarizeVersion(_ commit: String) async {
    guard VersionSummarizer.enabled, let rpc, !versionSummaryTried.contains(commit) else { return }
    versionSummaryTried.insert(commit)
    versionSummaryBusy.insert(commit)
    defer { versionSummaryBusy.remove(commit) }
    guard let config = VersionSummarizer.config() else {
      versionSummaryErrors[commit] = String(localized: "この接続では版の要約を作りません。")
      return
    }
    do {
      await refreshVersions()
      guard let c = versionGraph?.commitList.first(where: { $0.id == commit }), (c.changeSummary ?? "").isEmpty else { return }
      let diff = try await rpc.versionDiff(commit: commit)
      // Nothing a summary may read changed (the layout, data files).
      guard !diff.text.isEmpty else { return }
      guard let project = try await RPCToolTransport(session: self).project() else { return }
      let text = try await VersionSummarizer.summarize(project: project, commit: commit, config: config)
      if text.isEmpty {
        versionSummaryErrors[commit] = String(localized: "モデルが文を返しませんでした。")
      } else {
        _ = try await rpc.versionSummary(opId: UUID(), origin: "ui", commit: commit, summary: text)
        await refreshVersions()
      }
    } catch {
      versionSummaryErrors[commit] = error.localizedDescription
    }
  }

  /// Makes the project a repository (a first version of what is saved).
  func makeRepository() async throws {
    guard let rpc else { throw RPCError.disconnected }
    _ = try await rpc.versionInit(opId: UUID(), origin: "ui")
    await refreshVersions()
  }

  /// Names what is saved now as a version (a commit and an annotated tag).
  func markVersion(name: String, message: String) async throws {
    guard let rpc else { throw RPCError.disconnected }
    if dirty { await save() }
    if dirty { throw VersionError.unsaved }
    _ = try await rpc.versionMark(opId: UUID(), origin: "ui", name: name, message: message)
    await refreshVersions()
  }

  /// Starts from `commit`: the documents become that version's; a change and
  /// run from there opens a branch, and nothing later is lost.
  func startFrom(_ commit: String) async throws {
    guard let rpc else { throw RPCError.disconnected }
    if runActive { throw VersionError.running }
    if dirty { throw VersionError.unsaved }
    _ = try await rpc.versionCheckout(baseRev: rev, opId: UUID(), origin: "ui", commit: commit)
    await refreshVersions()
  }
}

enum VersionError: LocalizedError {
  case unsaved, running
  var errorDescription: String? {
    switch self {
    case .unsaved: return String(localized: "未保存の変更があります。保存してから選んでください。")
    case .running: return String(localized: "実行中は別の版に移れません。")
    }
  }
}
