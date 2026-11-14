import Foundation
import TsugumiAgentClient
import TsugumiRPC

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

  /// Ask agentd for the note to display. It owns policy, generation and storage.
  func loadVersionNote(_ commit: String) async {
    guard rpc != nil else { return }
    let request = UUID()
    versionNoteRequests[commit] = request
    versionNoteBusy.insert(commit)
    versionNoteErrors[commit] = nil
    defer {
      if versionNoteRequests[commit] == request {
        versionNoteBusy.remove(commit)
        versionNoteRequests[commit] = nil
      }
    }
    do {
      await CorePreferences.shared.reload()
      guard let project = try await RPCToolTransport(session: self).project() else { return }
      let note = try await CoreAgentClient.shared.versionNote(project: project.id, commit: commit,
        language: AppPreferences.Language.running.rawValue)
      if versionNoteRequests[commit] == request { versionNotes[commit] = note }
    } catch {
      if versionNoteRequests[commit] == request { versionNoteErrors[commit] = error.localizedDescription }
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
