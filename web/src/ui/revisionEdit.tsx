import { L } from '../i18n'
import { Observable, useObserve } from '../model/observable'
import { errorText, ProjectSession } from '../model/session'
import { D, RPCFailure } from '../shared/rpc'
import { Button } from './controls'

/** A rejected working copy stays outside the autosaved session draft. */
export class RevisionEdit extends Observable {
  baseRev = 0n
  conflictRev?: bigint
  keptText?: string
  error?: string
  applying = false
  savedRev?: bigint

  reload(rev: bigint) {
    this.baseRev = rev
    this.conflictRev = undefined
    this.error = undefined
    this.changed()
  }
  keepAndReload(text: string, load: () => void) {
    this.keptText = text
    load()
    this.changed()
  }
  conflict(rev: bigint) {
    this.conflictRev ??= rev
    this.changed()
  }
  async apply(session: ProjectSession, overwrite: boolean, change: (s: ProjectSession) => void): Promise<boolean> {
    if (this.applying || (this.conflictRev !== undefined && !overwrite)) return false
    const revision = overwrite ? this.conflictRev ?? this.baseRev : this.baseRev
    this.applying = true
    this.changed()
    try {
      this.savedRev = await session.applyEdit(revision, change)
      this.reload(this.savedRev ?? session.rev)
      return true
    } catch (e) {
      if (e instanceof RPCFailure && (e.kind === D.FailureKind.revision_conflict || e.kind === D.FailureKind.hash_conflict)) {
        this.conflictRev = session.rev
        this.error = undefined
      } else this.error = errorText(e)
      return false
    } finally {
      this.applying = false
      this.changed()
    }
  }
}

export function RevisionEditNotice({ edit, text, overwrite, reload }: { edit: RevisionEdit; text: () => string; overwrite: () => void; reload: () => void }) {
  useObserve(edit)
  return <div className="col gap6">
    {edit.conflictRev !== undefined && <>
      <span className="callout" style={{ color: 'var(--orange)' }}>{L('編集中に別の変更が保存されました。上書きするか、編集内容を保持して最新版を読み込んでください。')}</span>
      <div className="row gap8">
        <Button disabled={edit.applying} onClick={overwrite}>{L('上書き')}</Button>
        <Button disabled={edit.applying} onClick={() => edit.keepAndReload(text(), reload)}>{L('編集内容を保持して再読込')}</Button>
      </div>
    </>}
    {edit.error && <span className="callout selectable" style={{ color: 'var(--red)' }}>{edit.error}</span>}
    {edit.keptText !== undefined && <details><summary>{L('再適用のために保持した編集内容')}</summary><pre className="selectable" style={{ maxHeight: 140, overflow: 'auto' }}>{edit.keptText}</pre></details>}
  </div>
}

/** The value and revision actually visible when an inline edit starts. */
export class InlineParamEdit extends RevisionEdit {
  initial = ''
  begin(value: string, rev: bigint) { this.initial = value; this.reload(rev) }
  async commit(session: ProjectSession, text: string, overwrite: boolean, change: (s: ProjectSession) => void) {
    if (text === this.initial) return true
    const ok = await this.apply(session, overwrite, change)
    if (ok) this.initial = text
    return ok
  }
}
