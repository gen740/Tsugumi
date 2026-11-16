import assert from 'node:assert/strict'
import { ProjectSession } from '../src/model/session'
import { clone, get, numText, parse } from '../src/shared/json'
import { R } from '../src/shared/rpc'

function session() {
  const s = new ProjectSession('/merge', 'merge', 'merge')
  s.connected = true
  s.scheduleSave = () => {}
  s.project = parse('{"settings":{"n":1},"canvas":{"cards":[],"connections":[]}}')
  s.confirmedProject = clone(s.project)
  s.layout = parse('{}'); s.confirmedLayout = clone(s.layout)
  const saves: R.DocumentChangeT[][] = []
  s.client = { save: async (base: bigint, op: string, origin: string, changes: R.DocumentChangeT[]) => {
    saves.push(changes)
    const delta = Object.assign(new R.DocumentDeltaT(), { baseRev: base, rev: base + 1n, opId: op, origin })
    for (const change of changes) if (change.valueType === R.DocumentChangeValue.SetProject) delta.projectJson = (change.value as R.SetProjectT).json
    return Object.assign(new R.SaveResultT(), { rev: delta.rev, delta })
  } } as never
  const save = () => (s as unknown as { saveOnce(): Promise<void> }).saveOnce()
  const mine = () => s.edit(x => { x.project = parse('{"settings":{"n":2},"canvas":{"cards":[],"connections":[]}}') })
  const remote = (n: number) => { s.rev++; s.confirmedProject = parse(`{"settings":{"n":${n}},"canvas":{"cards":[],"connections":[]}}`) }
  return { s, saves, save, mine, remote }
}
{
  const { s, saves, save, mine, remote } = session()
  mine(); remote(3)
  await save()
  assert.equal(saves.length, 0, 'a conflicting run condition is never silently saved')
  assert.equal(s.mergeEdit.conflictRev, 1n)
  assert.equal(numText(get(s.project, 'settings', 'n')), '2')
  s.resolveMergeConflict()
  await save()
  assert.equal(saves.length, 1, 'the reviewed choice goes through one project save')
  assert.equal(numText(get(s.confirmedProject, 'settings', 'n')), '2')
}
{
  const { s, saves, save, mine, remote } = session()
  mine(); remote(3); await save()
  remote(4)
  s.resolveMergeConflict(); await save()
  assert.equal(saves.length, 0, 'approval never applies to a newer revision')
  assert.equal(s.mergeEdit.conflictRev, 2n)
  const text = s.draftSummary()
  s.mergeEdit.keepAndReload(text, () => s.discardDraft())
  assert.equal(s.mergeEdit.keptText, text)
  assert.equal(numText(get(s.project, 'settings', 'n')), '4')
  assert.equal(s.dirty, false)
}
{
  const { s, save, mine, remote } = session()
  mine(); remote(3)
  const original = s.prepareMerge
  let finish!: () => void
  let started!: () => void
  const onStart = new Promise<void>(resolve => { started = resolve })
  s.prepareMerge = async (...args) => { const reply = await original.apply(s, args); return new Promise(resolve => { finish = () => resolve(reply); started() }) }
  const pending = save()
  await onStart
  s.edit(x => { x.project = parse('{"settings":{"n":5},"canvas":{"cards":[],"connections":[]}}') })
  finish(); await pending
  assert.equal(numText(get(s.project, 'settings', 'n')), '5', 'a late host reply cannot replace an edit made while waiting')
  assert.equal(s.mergeEdit.conflictRev, undefined)
}
{
  const { s, mine, remote } = session()
  mine(); remote(3)
  // Undo over another editor's conflicting setting uses the same choice.
  s.project = clone(s.confirmedProject)
  await s.undo()
  assert.equal(s.mergeEdit.conflictRev, 1n)
  assert.equal(s.undoStack.length, 1)
  s.resolveMergeConflict()
  assert.equal(numText(get(s.project, 'settings', 'n')), '1')
  assert.equal(s.undoStack.length, 0)
  assert.equal(s.redoStack.length, 1)
}
console.log('unsaved host merge: passed')
