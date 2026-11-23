import SwiftUI
import TsugumiRPC

/// An editor's revision and explicit conflict choice. Text stays in the
/// editor until one transaction succeeds; rejected text is never autosaved.
@MainActor @Observable
final class RevisionEdit {
  var baseRev: UInt64 = 0
  var conflictRev: UInt64?
  var keptText: String?
  var error: String?
  var applying = false
  var savedRev: UInt64?
  private var conflictDocuments: [String: JSONValue] = [:]
  private var conflictHashes: [String: String] = [:]

  /// Freeze the comparison values when the conflict is shown, not on retry.
  private func recordConflict(_ session: ProjectSession) {
    conflictRev = session.rev
    conflictDocuments = ["tsugumi.json": session.project, "layout.bin": session.layout]
    func definitions(_ prefix: String, _ values: [String: DefinitionDraft]) {
      for (id, d) in values {
        let dir = prefix + "definitions/" + id + "/"
        conflictDocuments[dir + "_card.json"] = d.card
        let notes = Documents.splitCardNotes(d.meta)
        conflictDocuments[dir + "#notes"] = d.meta
        conflictDocuments[dir + "_comments.json"] = notes.comments
        if let readme = notes.readme { conflictDocuments[dir + "README.md"] = .string(readme) }
      }
    }
    definitions("", session.definitionDrafts)
    for (prefix, external) in session.externals {
      conflictDocuments[prefix + "tsugumi.json"] = external.project
      conflictDocuments[prefix + "layout.bin"] = external.layout
      definitions(prefix, external.definitions)
    }
    conflictHashes = session.sources.mapValues { $0.hash }
    error = nil
  }

  func jsonBase(_ file: String, initial: JSONValue?, overwrite: Bool) -> JSONValue? {
    overwrite ? conflictDocuments[file] : initial
  }

  func hashBase(_ file: String, initial: String?, overwrite: Bool) -> String? {
    overwrite ? conflictHashes[file] : initial
  }

  func reload(_ rev: UInt64) {
    baseRev = rev
    conflictRev = nil
    conflictDocuments = [:]
    conflictHashes = [:]
    error = nil
  }

  func keepAndReload(_ text: String, _ load: () -> Void) {
    keptText = text
    load()
  }

  func conflict(_ rev: UInt64) {
    if conflictRev == nil { conflictRev = rev }
  }

  func apply(_ session: ProjectSession, _ change: (ProjectSession) -> Void) async -> Bool {
    await applyOperation(session) { try await session.applyMutation(change) }
  }

  func applyOperation(_ session: ProjectSession, _ operation: () async throws -> UInt64) async -> Bool {
    guard !applying else { return false }
    applying = true
    defer { applying = false }
    do {
      savedRev = try await operation()
      reload(savedRev ?? session.rev)
      return true
    } catch let failure as RPCFailure where failure.kind == .revisionConflict || failure.kind == .hashConflict || failure.kind == .valueConflict || failure.kind == .sourceChanged {
      recordConflict(session)
    } catch {
      self.error = String(describing: error)
    }
    return false
  }
  func replaceDocuments(_ session: ProjectSession, overwrite: Bool = false, _ documents: [(String, JSONValue?)]) async -> Bool {
    guard !applying else { return false }
    applying = true
    defer { applying = false }
    do {
      savedRev = try await session.replaceDocuments(overwrite ? (conflictRev ?? baseRev) : baseRev, documents)
      reload(savedRev ?? session.rev)
      return true
    } catch let failure as RPCFailure where failure.kind == .revisionConflict || failure.kind == .hashConflict || failure.kind == .valueConflict || failure.kind == .sourceChanged {
      recordConflict(session)
      return false
    } catch { self.error = String(describing: error); return false }
  }

}

struct RevisionEditNotice: View {
  let edit: RevisionEdit
  let text: () -> String
  let overwrite: () -> Void
  let reload: () -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      if edit.conflictRev != nil {
        Text("編集中に別の変更が保存されました。上書きするか、編集内容を保持して最新版を読み込んでください。")
          .font(.callout).foregroundStyle(.orange)
        HStack {
          Button("上書き") { overwrite() }.disabled(edit.applying)
          Button("編集内容を保持して再読込") {
            edit.keepAndReload(text(), reload)
          }.disabled(edit.applying)
        }
      }
      if let error = edit.error { Text(error).font(.callout).foregroundStyle(.red).textSelection(.enabled) }
      if let kept = edit.keptText {
        DisclosureGroup("再適用のために保持した編集内容") {
          ScrollView { Text(kept).font(.caption.monospaced()).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }
            .frame(maxHeight: 140)
        }
      }
    }
  }
}

/// The value and revision actually visible when an inline edit starts.
@MainActor @Observable
final class InlineParamEdit {
  let revision = RevisionEdit()
  private(set) var initial = ""
  private struct FieldBase: Equatable {
    let file: String
    let selector: [JSONValue]
    let value: JSONValue?
  }
  private var fieldBase: FieldBase?

  func beginField(_ text: String, session: ProjectSession, file: String, selector: [JSONValue], value: JSONValue?) {
    begin(text, rev: session.rev)
    fieldBase = FieldBase(file: file, selector: selector, value: value)
  }

  func commitField(_ session: ProjectSession, text: String, overwrite: Bool = false,
                   _ change: (inout JSONValue?) -> Void) async -> Bool {
    guard let base = fieldBase else { return false }
    var before = base.value
    if overwrite {
      before = revision.jsonBase(base.file, initial: nil, overwrite: true)
      for step in base.selector {
        if let field = step.string { before = before?[field] }
        else if let match = step.object {
          before = before?.array?.first { item in match.allSatisfy { item[$0.0] == $0.1 } }
        }
      }
    }
    var after = before ?? base.value
    change(&after)
    let ok = await commit(session, text: text) { s in
      s.appendEdits([s.fieldEdit(base.file, base.selector, old: before, new: after)])
    }
    if ok, fieldBase == base { fieldBase = FieldBase(file: base.file, selector: base.selector, value: after) }
    return ok
  }

  func commitParam(_ session: ProjectSession, card: CardKey, name: String, literal: String, overwrite: Bool) async -> Bool {
    let expectedRev = overwrite ? (revision.conflictRev ?? revision.baseRev) : revision.baseRev
    return await revision.applyOperation(session) { try await session.setParam(card, name, literal, expectedRev: expectedRev) }
  }

  func begin(_ value: String, rev: UInt64) { initial = value; revision.reload(rev) }
  func commit(_ session: ProjectSession, text: String,
              _ change: (ProjectSession) -> Void) async -> Bool {
    if text == initial { return true }
    let ok = await revision.apply(session, change)
    if ok { initial = text }
    return ok
  }
}
