// Settings copies never inherit a newer session revision without a user choice.
import assert from 'node:assert/strict'
import { clone, get, parse, str } from '../src/shared/json'
import { D, R, RPCFailure } from '../src/shared/rpc'
import { ProjectSession } from '../src/model/session'
import { CardEditorModel } from '../src/ui/cardSettings'
import { InlineParamEdit, RevisionEdit } from '../src/ui/revisionEdit'

function session() {
  const s = new ProjectSession('/p', 'k', 'p')
  s.connected = true
  s.scheduleSave = () => {}
  s.project = parse('{"name":"base","canvas":{"cards":[{"id":"a"}],"connections":[]}}')
  s.confirmedProject = parse('{"name":"base","canvas":{"cards":[{"id":"a"}],"connections":[]}}')
  s.layout = parse('{}')
  s.confirmedLayout = parse('{}')
  s.confirmedDefinitions.set('a', parse('{"template":"draft","symbol":"base"}'))
  const requests: bigint[] = []
  s.client = {
    save: async (base: bigint, op: string, origin: string, changes: R.DocumentChangeT[]) => {
      requests.push(base)
      const delta = new R.DocumentDeltaT()
      Object.assign(delta, { baseRev: base, rev: base + 1n, executionRev: 0n, opId: op, origin })
      for (const c of changes) {
        if (c.valueType === R.DocumentChangeValue.SetProject) delta.projectJson = (c.value as R.SetProjectT).json
        if (c.valueType === R.DocumentChangeValue.SetLayout) delta.layoutJson = (c.value as R.SetLayoutT).json
        if (c.valueType === R.DocumentChangeValue.PutDefinition) {
          const put = c.value as R.PutDefinitionT
          delta.definitionsUpsert.push(Object.assign(new R.DefinitionT(), put))
        }
      }
      return Object.assign(new R.SaveResultT(), { rev: delta.rev, delta })
    },
  } as never
  return { s, requests }
}

// The raw JSON and project forms use the same revision-aware apply entry.
for (const document of ['definition', 'project']) {
  const { s, requests } = session()
  const edit = new RevisionEdit()
  edit.reload(s.rev)
  const mine = parse(document === 'definition' ? '{"template":"draft","symbol":"mine"}' : '{"name":"mine"}')
  const apply = (overwrite = false) => edit.apply(s, overwrite, (x) => {
    if (document === 'definition') x.setCardDefinition('a', { card: mine })
    else x.project = mine
  })
  s.rev = 1n // The Agent saves while this editor is open.
  assert.equal(await apply(), false)
  assert.equal(edit.conflictRev, 1n)
  assert.equal(s.dirty, false)
  assert.deepEqual(requests, [])
  assert.equal(await apply(), false, 'Apply alone never chooses overwrite')
  s.rev = 2n // The Agent saves again after the conflict choice was shown.
  assert.equal(await apply(true), false, 'an overwrite choice applies only to the reviewed revision')
  assert.equal(edit.conflictRev, 2n)
  assert.equal(await apply(true), true)
  assert.deepEqual(requests, [2n])
  assert.equal(s.dirty, false, 'the editor save does not leave a stale autosave draft')
  assert.equal(document === 'definition' ? str(get(s.cardDefinition('a'), 'symbol')) : str(get(s.project, 'name')), 'mine')
  s.undo()
  assert.equal(document === 'definition' ? str(get(s.cardDefinition('a'), 'symbol')) : str(get(s.project, 'name')), 'base', 'editor apply remains undoable')
}

// A save racing after the client's check reaches RPC with the editor's base,
// and rejection never puts that text into a later autosave.
{
  const { s, requests } = session()
  const edit = new RevisionEdit()
  edit.reload(s.rev)
  s.client = {
    save: async (base: bigint) => {
      requests.push(base)
      s.rev = 1n
      throw new RPCFailure({ kind: D.FailureKind.revision_conflict, currentRev: 1n })
    },
  } as never
  assert.equal(await edit.apply(s, false, (x) => x.setCardDefinition('a', { card: parse('{"symbol":"mine"}') })), false)
  assert.deepEqual(requests, [0n])
  assert.equal(s.dirty, false)
  await (s as unknown as { saveOnce(): Promise<void> }).saveOnce()
  assert.deepEqual(requests, [0n], 'no automatic retry of rejected editor text')
  assert.equal(str(get(s.cardDefinition('a'), 'symbol')), 'base')
}

// The ordinary Card editor follows untouched fields and asks before a clash,
// including a clash discovered by Apply before React's follow effect ran.
{
  const { s, requests } = session()
  const m = new CardEditorModel(s, 'a')
  m.definition = parse('{"template":"draft","symbol":"mine"}')
  s.rev = 1n
  s.confirmedDefinitions.set('a', parse('{"template":"draft","symbol":"agent","maxRuns":3}'))
  assert.equal(await m.apply(), false)
  assert.deepEqual(requests, [])
  assert.equal(str(get(m.definition, 'symbol')), 'mine')
  assert.deepEqual(m.conflicts, ['_card.json: symbol'])
  assert.equal(await m.apply(true), true)
  assert.deepEqual(requests, [1n])
  assert.equal(str(get(s.cardDefinition('a'), 'symbol')), 'mine')
  assert.ok(get(s.cardDefinition('a'), 'maxRuns'), 'untouched settings follow the Agent')
}
console.log('stale settings apply: passed')

// Creating over a newly arrived source and deleting a changed source make
// the same explicit choice, with the hash of the reviewed revision.
for (const operation of ['create', 'remove']) {
  const { s, requests } = session()
  const p = 'definitions/a/source.txt'
  if (operation === 'remove') s.sources.set(p, { hash: 'base', size: 4n })
  const m = new CardEditorModel(s, 'a')
  if (operation === 'create') m.createFile('source.txt', 'mine')
  else m.removeFile(p)
  s.rev = 1n
  s.sources.set(p, { hash: 'agent', size: 5n })
  s.client = {
    readFile: async () => new TextEncoder().encode('agent'),
    save: async (base: bigint, _op: string, _origin: string, changes: R.DocumentChangeT[]) => {
      requests.push(base)
      assert.equal((changes[0].value as R.PutSourceT | R.RemoveSourceT).baseHash, 'agent')
      return Object.assign(new R.SaveResultT(), { rev: 2n })
    },
  } as never
  assert.equal(await m.apply(), false)
  assert.deepEqual(m.conflicts, ['source.txt'])
  assert.deepEqual(requests, [])
  assert.equal(await m.apply(true), true)
  assert.deepEqual(requests, [1n])
}

// Reload keeps the exact previous JSON (including invalid or unapplied text)
// available to copy, while the editor's visible content takes the newer base.
{
  const { s } = session()
  const edit = new RevisionEdit()
  edit.reload(s.rev)
  let text = '{"symbol": "mine",\n'
  const mine = text
  s.rev = 1n
  edit.conflict(s.rev)
  edit.keepAndReload(text, () => { text = '{"symbol":"agent"}'; edit.reload(s.rev) })
  assert.equal(text, '{"symbol":"agent"}')
  assert.equal(edit.keptText, mine)
  assert.equal(edit.conflictRev, undefined)
  assert.equal(edit.baseRev, 1n)
}

// An edited comment is a definition edit too, including while a run is active.
{
  const { s, requests } = session()
  s.confirmedDefinitionMeta.set('a', parse('{"comments":[{"id":"c","text":"base"}]}'))
  const edit = new RevisionEdit()
  edit.reload(s.rev)
  s.rev = 1n
  s.confirmedDefinitionMeta.set('a', parse('{"comments":[{"id":"c","text":"agent"}]}'))
  const apply = (overwrite = false) => edit.apply(s, overwrite, (x) => x.setCommentText('a', 'c', 'mine'))
  assert.equal(await apply(), false)
  assert.equal(s.comments('a')[0].text, 'agent')
  s.runStatus = Object.assign(new R.RunStatusT(), { runId: 'active', state: D.RunState.running })
  assert.ok(s.runActive)
  assert.equal(await apply(true), true)
  assert.deepEqual(requests, [1n])
  assert.equal(s.comments('a')[0].text, 'mine')
}

// A newer Agent save can arrive before the reply to our successful save.
// Reload must not give our older source text the newer source's hash.
{
  const { s } = session()
  const p = 'definitions/a/source.txt'
  s.sources.set(p, { hash: 'base', size: 4n })
  let current = 'base'
  s.client = {
    readFile: async () => new TextEncoder().encode(current),
    save: async () => {
      s.rev = 2n
      s.sources.set(p, { hash: 'newer', size: 5n })
      current = 'newer'
      return Object.assign(new R.SaveResultT(), { rev: 1n })
    },
  } as never
  const m = new CardEditorModel(s, 'a')
  await m.text(p)
  m.setText(p, 'mine')
  assert.equal(await m.apply(), true)
  assert.equal(m.loadedText(p), undefined)
  assert.equal(await m.text(p), 'newer')
}

// Opening and closing an unchanged Draft never writes the Agent's newer text/name.
for (const field of ['content', 'name']) {
  const { s, requests } = session()
  s.confirmedDefinitionMeta.set('a', parse('{"name":"base","content":"base"}'))
  s.startDraftEdit('a')
  s.rev = 1n
  s.confirmedDefinitionMeta.set('a', parse('{"name":"agent","content":"agent"}'))
  await s.finishDraftEdit()
  assert.equal(s.inlineDraft, undefined)
  assert.equal(s.dirty, false)
  assert.deepEqual(requests, [])
  assert.equal(str(get(s.cardMeta('a'), field)), 'agent')

  s.startDraftEdit('a')
  s.setDraftEdit({ [field]: 'mine' })
  s.rev = 2n
  s.confirmedDefinitionMeta.set('a', parse('{"name":"newer","content":"newer"}'))
  await s.finishDraftEdit()
  assert.equal(s.inlineDraft?.revision.conflictRev, 2n)
  assert.equal(s.inlineDraft?.[field], 'mine')
  assert.equal(s.dirty, false)
  assert.deepEqual(requests, [])
  s.rev = 3n
  await s.finishDraftEdit(true)
  assert.equal(s.inlineDraft?.revision.conflictRev, 3n, 'overwrite is bound to the reviewed revision')
  await s.finishDraftEdit(true)
  assert.equal(s.inlineDraft, undefined)
  assert.deepEqual(requests, [3n])
  assert.equal(str(get(s.cardMeta('a'), field)), 'mine')
  assert.equal(str(get(s.cardMeta('a'), field === 'name' ? 'content' : 'name')), 'newer', 'untouched fields follow the Agent')
}
console.log('Draft no-op, conflict and explicit overwrite: passed')

function deferred() {
  let resolve!: () => void
  const promise = new Promise<void>(done => { resolve = done })
  return { promise, resolve }
}

// Return/focus loss can submit twice while a preceding canvas autosave is in
// flight. Switching Drafts, running and leaving must join that save too.
for (const action of ['done', 'switch', 'run', 'leave']) {
  const { s, requests } = session()
  s.project = s.confirmedProject = parse('{"canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}}')
  s.confirmedDefinitions.set('b', parse('{"template":"draft"}'))
  s.scheduleSave = ProjectSession.prototype.scheduleSave.bind(s)
  const entered = deferred(), release = deferred()
  const client = s.client!
  const save = client.save.bind(client)
  let first = true, ran = false
  client.save = async (...args) => {
    if (first) { first = false; entered.resolve(); await release.promise }
    return save(...args)
  }
  client.run = async () => {
    assert.equal(str(get(s.cardMeta('a'), 'content')), 'count words')
    assert.equal(s.inlineDraft, undefined)
    ran = true
    return new R.RunAcceptedT()
  }
  s.edit(x => x.setPosition('a', { x: 50, y: 50 }))
  await entered.promise
  await s.startDraftEdit('a')
  s.setDraftEdit({ name: '日本語', content: 'count words' })
  const done = s.finishDraftEdit() // the UI intentionally does not await this callback
  const next = action === 'switch' ? s.startDraftEdit('b')
    : action === 'run' ? s.run([], false)
    : action === 'leave' ? s.settleBeforeLeaving(async () => assert.fail('no conflict'))
    : s.finishDraftEdit()
  release.resolve()
  await Promise.all([done, next])
  assert.deepEqual(requests, [0n, 1n], action)
  assert.equal(str(get(s.cardMeta('a'), 'content')), 'count words', action)
  assert.equal(s.displayName('a'), '日本語', action)
  assert.equal(s.inlineDraft?.card, action === 'switch' ? 'b' : undefined, action)
  assert.equal(s.inlineDraft?.revision.conflictRev, undefined, action)
  assert.equal(ran, action === 'run')
}

// Text typed after submitting, while RPC is pending, is saved before the edit
// ends. A second focus-loss callback must not silently skip those keystrokes.
{
  const { s, requests } = session()
  const entered = deferred(), release = deferred()
  const client = s.client!, save = client.save.bind(client)
  let first = true
  client.save = async (...args) => {
    if (first) { first = false; entered.resolve(); await release.promise }
    return save(...args)
  }
  await s.startDraftEdit('a')
  s.setDraftEdit({ content: 'first' })
  const done = s.finishDraftEdit()
  await entered.promise
  s.setDraftEdit({ content: 'last keystrokes' })
  const blurred = s.finishDraftEdit()
  release.resolve()
  await Promise.all([done, blurred])
  assert.deepEqual(requests, [0n, 1n])
  assert.equal(str(get(s.cardMeta('a'), 'content')), 'last keystrokes')
  assert.equal(s.inlineDraft, undefined)
}

// A conflicting Draft cannot be replaced, run with stale content or silently
// dropped when leaving. The explicit discard choice still works.
{
  const { s, requests } = session()
  await s.startDraftEdit('a')
  s.setDraftEdit({ content: 'keep my text' })
  s.rev = 1n
  await s.startDraftEdit('b')
  assert.equal(s.inlineDraft?.card, 'a')
  await assert.rejects(s.run([], false), RPCFailure)
  assert.equal(await s.settleBeforeLeaving(async () => false), false)
  assert.equal(s.hasUnsavedEdits, true)
  assert.equal(s.inlineDraft?.content, 'keep my text')
  assert.deepEqual(requests, [])
  assert.equal(await s.settleBeforeLeaving(async () => true), true)
  assert.equal(s.hasUnsavedEdits, false)
}
console.log('Draft pending autosave, repeated finish, switch, run and leave: passed')

// Another settings editor in the same session uses the same origin. Only a
// canvas autosave can advance the Draft's base, not that editor's transaction.
{
  const { s, requests } = session()
  await s.startDraftEdit('a')
  assert.equal(s.hasUnsavedEdits, false, 'an unchanged open editor needs no unload warning')
  s.setDraftEdit({ content: 'mine' })
  const other = new RevisionEdit(); other.reload(s.rev)
  assert.equal(await other.apply(s, false, x => x.updateMeta('a', m => m.set('content', 'other editor'))), true)
  await s.finishDraftEdit()
  assert.equal(s.inlineDraft?.content, 'mine')
  assert.equal(s.inlineDraft?.revision.conflictRev, 1n)
  assert.equal(str(get(s.cardMeta('a'), 'content')), 'other editor')
  assert.deepEqual(requests, [0n])
}

// The sticky-note and group-title forms submit through the same unchanged
// guard and raw layout mutations; no rejected value reaches autosave.
for (const kind of ['notes', 'groups']) {
  const { s, requests } = session()
  const install = (value: string) => {
    s.layout = parse(JSON.stringify({ [kind]: [{ id: 'row', text: value, name: value }] }))
    s.confirmedLayout = clone(s.layout)
  }
  install('base')
  const edit = new InlineParamEdit(); edit.begin('base', s.rev)
  s.rev = 1n; install('agent')
  const change = (value: string) => (x: ProjectSession) => {
    if (kind === 'notes') x.changeNote('row', n => n.set('text', value))
    else x.updateGroups([], groups => (groups[0] as import('../src/shared/json').JObj).set('name', value))
  }
  assert.equal(await edit.commit(s, 'base', false, change('base')), true)
  assert.deepEqual(requests, [])
  assert.equal(await edit.commit(s, 'mine', false, change('mine')), false)
  assert.equal(s.dirty, false)
  assert.equal(edit.conflictRev, 1n)
  assert.equal(await edit.commit(s, 'mine', true, change('mine')), true)
  assert.deepEqual(requests, [1n])
  assert.equal(kind === 'notes' ? str(get(s.note('row'), 'text')) : str(get(s.groups([])[0], 'name')), 'mine')
}
console.log('note and group edits retain their opening revision: passed')

{
  const { s } = session()
  s.startDraftEdit('a'); s.setDraftEdit({ content: 'keep this text' })
  const revision = s.inlineDraft!.revision
  revision.keepAndReload(s.inlineDraft!.content, () => s.startDraftEdit('a', revision))
  assert.equal(s.inlineDraft!.revision.keptText, 'keep this text')
  assert.equal(s.inlineDraft!.content, '')
}
