// Focused regressions: advance replies and external reads explicitly, without timers.
import assert from 'node:assert/strict'
import { Builder } from 'flatbuffers'
import * as H from '../shared/gen/tsugumi/host'
import { D, R, RPCFailure } from '../src/wire/rpc'
import { arr, get, num, numText, parse, serialize } from '../shared/json'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')
const { InlineParamEdit, RevisionEdit } = await import('../src/ui/revisionEdit')

function deferred<T>() {
  let resolve!: (value: T) => void
  const promise = new Promise<T>(r => { resolve = r })
  return { promise, resolve }
}
function session() {
  const s = new ProjectSession('/test', 'review', 'review') as any
  s.projectId = 'p'; s.connected = true; s.client = {}
  s.refreshDerivedLayout = () => {}; s.refreshVisibleDisplays = async () => {}
  s.apply(Object.assign(new R.ProjectSnapshotT(), { rev: 0n,
    projectJson: '{"canvas":{"cards":[{"id":"a"}],"connections":[]}}', layoutJson: '{}',
    definitions: [new R.DefinitionT('a', '{"template":"input","params":[{"name":"value","type":"int","data":1}]}', '{"content":"before"}')],
  }), true)
  return s
}
function inverse(name: string) { return new H.ProjectEditsT([new H.ProjectEditT(H.ProjectEditValue.ProjectFieldEdit,
  new H.ProjectFieldEditT('tsugumi.json', [new H.ProjectSelectorT('name')], new H.ProjectValueT(true, '"after"'), new H.ProjectValueT(true, JSON.stringify(name))))]) }
function saved(s: any, request: H.ProjectOperateRequestT, edits = inverse('inverse'), fields = {}) {
  const builder = new Builder(); builder.finish(edits.pack(builder))
  const d = Object.assign(new R.DocumentDeltaT(), { baseRev: s.rev, rev: s.rev + 1n, opId: request.opId, origin: 'ui', ...fields })
  return Object.assign(new R.SaveResultT(), { rev: d.rev, delta: d, operationData: [...builder.asUint8Array()] })
}

// An open parameter and Draft retain the values the person began editing.
// A separate writer changes them before save; retrying explicitly overwrites.
{
  const s = session(), edit = new InlineParamEdit()
  edit.begin('1', s.rev)
  edit.paramBase = { parameter: arr(get(s.cardDefinition('a'), 'params'))![0] }
  const changed = Object.assign(new R.DocumentDeltaT(), { baseRev: 0n, rev: 1n, origin: 'agent',
    definitionsUpsert: [new R.DefinitionT('a', '{"template":"input","params":[{"name":"value","type":"int","data":2}]}', '{"content":"before"}')] })
  s.applyDelta(changed)
  const guards: number[] = []
  s.operateProject = async (request: H.ProjectOperateRequestT) => {
    const edit = (request.body as H.ProjectEditsT).edits[0]
    const field = edit.value as H.ProjectFieldEditT | H.ProjectJSONEditT
    const before = parse(String(field.oldValue!.json))
    const old = edit.valueType === H.ProjectEditValue.ProjectFieldEdit ? Number(field.oldValue!.json)
      : Number(serialize(get(arr(get(before, 'params'))![0], 'data')!))
    guards.push(old)
    if (old !== 2) throw new RPCFailure({ kind: D.FailureKind.value_conflict })
    return saved(s, request)
  }
  assert.equal(await edit.commit(s, '3', false, x => x.updateParam('a', 'value', num(3), edit.paramBase)), false)
  assert.equal(edit.conflictRev, 1n, edit.error)
  assert.equal(await edit.commit(s, '3', true, x => x.updateParam('a', 'value', num(3))), true)
  assert.deepEqual(guards, [1, 2])

  const draft = session()
  await draft.startDraftEdit('a')
  draft.inlineDraft.content = 'mine'
  draft.applyDelta(Object.assign(new R.DocumentDeltaT(), { baseRev: 0n, rev: 1n, origin: 'agent',
    definitionsUpsert: [new R.DefinitionT('a', '{"template":"draft"}', '{"content":"agent","description":"unrelated"}')] }))
  const bases: string[] = []
  draft.operateProject = async (request: H.ProjectOperateRequestT) => {
    const json = (request.body as H.ProjectEditsT).edits[0].value as H.ProjectJSONEditT
    bases.push(JSON.parse(String(json.oldValue!.json)).content)
    if (bases.at(-1) !== 'agent') throw new RPCFailure({ kind: D.FailureKind.value_conflict })
    assert.equal(JSON.parse(String(json.newValue!.json)).description, 'unrelated')
    return saved(draft, request)
  }
  await draft.finishDraftEdit()
  assert.equal(draft.inlineDraft.revision.conflictRev, 1n)
  assert.equal(draft.inlineDraft.content, 'mine')
  await draft.finishDraftEdit(true)
  assert.equal(draft.inlineDraft, undefined)
  assert.deepEqual(bases, ['before', 'agent'])
}

// Source hashes are delivered before an internal Card's updated JSON is read.
// The next queued gesture must wait for that read, including notification-first.
for (const echoFirst of [false, true]) {
  const s = session()
  const prefix = 'externals/sub/'
  const card = (value: number) => `{"template":"input","params":[{"name":"value","type":"int","data":${value}}]}`
  s.confirmedLinks.set('sub', parse('{"external":"sub"}'))
  s.confirmedProject = parse('{"canvas":{"cards":[{"id":"sub"}],"connections":[]}}')
  s.externals.set(prefix, { project: parse('{"canvas":{"cards":[{"id":"a"}],"connections":[]}}'), layout: parse('{}'),
    definitions: new Map([['a', { card: parse(card(1)) }]]) })
  const readStarted = deferred<void>(), releaseRead = deferred<void>()
  let value = 2, calls = 0
  s.client.readFile = async (file: string) => {
    if (file.endsWith('/_card.json')) { readStarted.resolve(); await releaseRead.promise }
    return new TextEncoder().encode(file.endsWith('/tsugumi.json') ? '{"canvas":{"cards":[{"id":"a"}],"connections":[]}}' : card(value))
  }
  s.operateProject = async (request: H.ProjectOperateRequestT) => {
    const field = (request.body as H.ProjectEditsT).edits[0].value as H.ProjectFieldEditT
    assert.equal(Number(field.oldValue!.json), ++calls)
    value = calls + 1
    const result = saved(s, request, inverse('internal'), { sourcesUpsert: [
      Object.assign(new R.SourceInfoT(), { path: prefix + 'tsugumi.json', hash: 'project' }), Object.assign(new R.SourceInfoT(), { path: prefix + 'definitions/a/_card.json', hash: String(value) }),
    ] })
    if (echoFirst) s.applyDelta(result.delta)
    return result
  }
  s.setParam('sub/a', 'value', num(2))
  s.setParam('sub/a', 'value', num(3))
  await Promise.race([readStarted.promise, s.operationTask.then(() => { throw new Error('Operation completed before internal read: ' + s.saveError) })])
  assert.equal(calls, 1)
  releaseRead.resolve()
  await s.save()
  assert.equal(calls, 2)
  assert.equal(numText(get(arr(get(s.cardDefinition('sub/a'), 'params'))![0], 'data')), '3')
  assert.equal(s.saveError, undefined)
}

// History selection, transport, and stack updates share the regular edit queue.
// Two rapid Undos select distinct entries; an edit queued after Undo stays last.
{
  const s = session()
  s.undoStack = [{ operation: inverse('first') }, { operation: inverse('second') }]
  const first = deferred<void>(), release = deferred<void>()
  const selected: string[] = []
  s.operateProject = async (request: H.ProjectOperateRequestT) => {
    const edit = (request.body as H.ProjectEditsT).edits[0]
    const value = JSON.parse(String((edit.value as H.ProjectFieldEditT | H.ProjectJSONEditT).newValue!.json))
    selected.push(edit.valueType === H.ProjectEditValue.ProjectJSONEdit ? value.name : value)
    if (selected.length === 1) { first.resolve(); await release.promise }
    return saved(s, request, inverse('redo-' + selected.at(-1)))
  }
  const undo1 = s.undo(), undo2 = s.undo()
  await first.promise
  release.resolve()
  await Promise.all([undo1, undo2])
  assert.deepEqual(selected, ['second', 'first'])
  assert.equal(s.undoStack.length, 0); assert.equal(s.redoStack.length, 2)
  await Promise.all([s.redo(), s.redo()])
  assert.deepEqual(selected.slice(2), ['redo-first', 'redo-second'])
  assert.equal(s.undoStack.length, 2); assert.equal(s.redoStack.length, 0)
  await Promise.all([s.undo(), s.applyMutation((x: any) => x.setProjectField('name', 'edit'))])
  assert.equal(s.undoStack.length, 2)
  assert.equal(s.redoStack.length, 0)
  const last = s.undoStack.at(-1).operation.edits[0].value as H.ProjectFieldEditT
  assert.equal(JSON.parse(String(last.newValue!.json)), 'redo-edit')
}

// Both current conflict kinds expose the explicit overwrite/reload choice.
for (const kind of [D.FailureKind.value_conflict, D.FailureKind.source_changed]) {
  const s = session(), edit = new RevisionEdit()
  s.operateProject = async () => { throw new RPCFailure({ kind }) }
  assert.equal(await edit.apply(s, false, x => x.setProjectField('name', 'mine')), false)
  assert.equal(edit.conflictRev, s.rev)
  assert.equal(edit.error, undefined)
  assert.equal(edit.applying, false)
}
console.log('reviewOperations: passed')
