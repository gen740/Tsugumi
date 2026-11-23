import { splitCardNotes } from '../model/documents'
import { arr, clone, get, J, jeq, JObj } from '../../shared/json'
import { L } from '../i18n'
import { Observable, useObserve } from '../model/observable'
import { errorText, ProjectSession } from '../model/session'
import { D, RPCFailure } from '../wire/rpc'
import { Button } from './controls'

/** A rejected working copy stays outside the autosaved session draft. */
export class RevisionEdit extends Observable {
  baseRev = 0n
  conflictRev?: bigint
  keptText?: string
  error?: string
  applying = false
  savedRev?: bigint
  private conflictDocuments = new Map<string, J | undefined>()
  private conflictHashes = new Map<string, string>()

  /** Freeze comparison values when the conflict is shown, not on retry. */
  private recordConflict(session: ProjectSession) {
    this.conflictRev = session.rev
    this.conflictDocuments = new Map([['tsugumi.json', clone(session.project)], ['layout.bin', clone(session.layout)]])
    const definitions = (prefix: string, values: ProjectSession['definitionDrafts']) => {
      for (const [id, d] of values) {
        const dir = `${prefix}definitions/${id}/`
        this.conflictDocuments.set(dir + '_card.json', clone(d.card))
        const { readme, comments } = splitCardNotes(d.meta)
        this.conflictDocuments.set(dir + '#notes', clone(d.meta))
        if (comments !== undefined) this.conflictDocuments.set(dir + '_comments.json', clone(comments))
        if (readme !== undefined) this.conflictDocuments.set(dir + 'README.md', readme)
      }
    }
    definitions('', session.definitionDrafts)
    for (const [prefix, external] of session.externals) {
      this.conflictDocuments.set(prefix + 'tsugumi.json', clone(external.project))
      this.conflictDocuments.set(prefix + 'layout.bin', clone(external.layout))
      definitions(prefix, external.definitions)
    }
    this.conflictHashes = new Map([...session.sources].map(([file, source]) => [file, source.hash]))
    this.error = undefined
  }

  jsonBase(file: string, initial: J | undefined, overwrite: boolean): J | undefined {
    return clone(overwrite ? this.conflictDocuments.get(file) : initial)
  }
  hashBase(file: string, initial: string | undefined, overwrite: boolean): string | undefined {
    return overwrite ? this.conflictHashes.get(file) : initial
  }

  reload(rev: bigint) {
    this.baseRev = rev
    this.conflictRev = undefined
    this.conflictDocuments.clear()
    this.conflictHashes.clear()
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
  async apply(session: ProjectSession, change: (s: ProjectSession) => void): Promise<boolean> {
    return this.applyOperation(session, () => session.applyMutation(change))
  }
  async applyOperation(session: ProjectSession, operation: () => Promise<bigint>): Promise<boolean> {
    if (this.applying) return false
    this.applying = true
    this.changed()
    try {
      this.savedRev = await operation()
      this.reload(this.savedRev ?? session.rev)
      return true
    } catch (e) {
      if (e instanceof RPCFailure && (e.kind === D.FailureKind.revision_conflict || e.kind === D.FailureKind.hash_conflict || e.kind === D.FailureKind.value_conflict || e.kind === D.FailureKind.source_changed)) {
        this.recordConflict(session)
      } else this.error = errorText(e)
      return false
    } finally {
      this.applying = false
      this.changed()
    }
  }
  async replaceDocuments(session: ProjectSession, documents: [string, J | undefined][], overwrite = false): Promise<boolean> {
    if (this.applying) return false
    this.applying = true; this.changed()
    try {
      this.savedRev = await session.replaceDocuments(overwrite ? this.conflictRev ?? this.baseRev : this.baseRev, documents)
      this.reload(this.savedRev)
      return true
    } catch (e) {
      if (e instanceof RPCFailure && (e.kind === D.FailureKind.revision_conflict || e.kind === D.FailureKind.hash_conflict || e.kind === D.FailureKind.value_conflict || e.kind === D.FailureKind.source_changed)) this.recordConflict(session)
      else this.error = errorText(e)
      return false
    }
    finally { this.applying = false; this.changed() }
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
  private fieldBase?: { file: string; selector: (string | JObj)[]; value?: J }
  beginField(text: string, session: ProjectSession, file: string, selector: (string | JObj)[], value: J | undefined) {
    this.begin(text, session.rev)
    this.fieldBase = { file, selector, value: clone(value) }
  }
  async commitField(session: ProjectSession, text: string, overwrite: boolean, change: (value: J | undefined) => J | undefined) {
    const base = this.fieldBase
    if (!base) return false
    let before = clone(base.value)
    if (overwrite) {
      before = this.jsonBase(base.file, undefined, true)
      for (const step of base.selector) {
        if (typeof step === 'string') before = get(before, step)
        else before = arr(before)?.find(item => step.entries().every(([k, v]) => jeq(get(item, k), v)))
      }
    }
    const after = change(clone(before === undefined ? base.value : before))
    const ok = await this.commit(session, text, s => s.appendEdits([s.fieldEdit(base.file, base.selector, before, after)]))
    if (ok && this.fieldBase === base) this.fieldBase = { ...base, value: clone(after) }
    return ok
  }
  async commitParam(session: ProjectSession, card: string, name: string, literal: string, overwrite: boolean) {
    const expectedRev = overwrite ? this.conflictRev ?? this.baseRev : this.baseRev
    return this.applyOperation(session, () => session.setParam(card, name, literal, expectedRev))
  }
  begin(value: string, rev: bigint) { this.initial = value; this.reload(rev) }
  async commit(session: ProjectSession, text: string, change: (s: ProjectSession) => void) {
    if (text === this.initial) return true
    const ok = await this.apply(session, change)
    if (ok) this.initial = text
    return ok
  }
}
