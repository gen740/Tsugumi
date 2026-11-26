import Foundation
import TsugumiAgentClient
import TsugumiRPC

/// The project's versions (tsugumid/runtime/src/versions.cc): a git repository whose
/// commits are what runs ran, with their results in notes and named
/// versions as tags.
extension ProjectSession {
  func refreshVersions() async {
    guard rpc != nil else { return }
    do {
      guard let project = try await RPCToolTransport(session: self).project() else { return }
      versionGraph = try await CoreHostClient.shared.versionGraph(project: project.id, language: AppPreferences.Language.running.rawValue)
    } catch { versionGraph = nil }
  }

  func versionDiff(commit: String, from: String?) async throws -> Tsugumi_RPC_VersionDiffT {
    guard let project = try await RPCToolTransport(session: self).project() else { throw RPCError.disconnected }
    return try await CoreHostClient.shared.versionDiff(project: project.id, commit: commit, from: from)
  }

  func loadVersionCanvas(_ commit: String) async {
    guard let rpc, versionCanvases[commit] == nil else { return }
    do {
      let raw = try await rpc.versionCanvas(commit: commit)
      let request = Tsugumi_Host_VersionCanvasRequestT()
      request.layout = raw.layout
      request.cards = raw.cards.compactMap { c in c.map { c in
        let value = Tsugumi_Host_VersionCanvasCardT(); value.id = c.id; value.kind = c.kind; value.fan = c.fan; return value
      } }
      request.wires = raw.wires.compactMap { w in w.map { w in
        let value = Tsugumi_Host_VersionCanvasWireT(); value.from = w.from; value.to = w.to; return value
      } }
      versionCanvases[commit] = try await CoreHostClient.shared.hostCall(.versioncanvasrequest, request)
    } catch { notice = .error(error.localizedDescription) }
  }

  func loadVersionComments(_ commit: String) async throws {
    guard versionComments[commit] == nil else { return }
    guard let project = try await RPCToolTransport(session: self).project() else { return }
    versionComments[commit] = try await CoreHostClient.shared.versionComments(project: project.id, commit: commit)
  }

  /// Ask hostd for the note to display. It owns policy, generation and storage.
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
      try await loadVersionComments(commit)
      await CorePreferences.shared.reload()
      guard let project = try await RPCToolTransport(session: self).project() else { return }
      let note = try await CoreHostClient.shared.versionNote(project: project.id, commit: commit,
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
    if dirty { throw VersionError.unsaved }
    _ = try await rpc.versionCheckout(baseRev: rev, opId: UUID(), origin: "ui", commit: commit)
    await refreshVersions()
  }
}

enum VersionError: LocalizedError {
  case unsaved
  var errorDescription: String? {
    switch self {
    case .unsaved: return String(localized: "未保存の変更があります。保存してから選んでください。")
    }
  }
}
