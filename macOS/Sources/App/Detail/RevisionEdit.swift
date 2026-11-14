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

  func reload(_ rev: UInt64) {
    baseRev = rev
    conflictRev = nil
    error = nil
  }

  func keepAndReload(_ text: String, _ load: () -> Void) {
    keptText = text
    load()
  }

  func conflict(_ rev: UInt64) {
    if conflictRev == nil { conflictRev = rev }
  }

  func apply(_ session: ProjectSession, overwrite: Bool = false, _ change: (ProjectSession) -> Void) async -> Bool {
    guard !applying else { return false }
    guard conflictRev == nil || overwrite else { return false }
    let revision = overwrite ? conflictRev ?? baseRev : baseRev
    applying = true
    defer { applying = false }
    do {
      savedRev = try await session.applyEdit(baseRev: revision, change)
      reload(session.rev)
      return true
    } catch let failure as RPCFailure where failure.kind == .revisionConflict || failure.kind == .hashConflict {
      conflictRev = session.rev
      error = nil
    } catch {
      self.error = String(describing: error)
    }
    return false
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
