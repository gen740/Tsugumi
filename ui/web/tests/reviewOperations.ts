import { definitionFixture, layoutFixture, textBytes } from './documentFixture'
// Focused regressions: advance replies and external reads explicitly, without timers.
import assert from 'node:assert/strict'
import { Builder } from 'flatbuffers'
import * as H from '../shared/gen/tsugumi/host'
import { D, R, RPCFailure, cardPath } from '../src/wire/rpc'
import { get, J, num, numText, obj, parse, serialize } from '../shared/json'

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
function resolved(card: string[], value: number) {
  return Object.assign(new R.ResolvedCardT(), {
    card: cardPath(card), parameters: [new R.CardParameterT('value', 'int', String(value))],
  })
}
function session() {
  const s = new ProjectSession('/test', 'review', 'review') as any
  s.projectId = 'p'; s.connected = true; s.client = {}
  s.refreshDerivedLayout = () => {}; s.refreshVisibleDisplays = async () => {}
  s.apply(Object.assign(new R.ProjectSnapshotT(), { rev: 0n,
    projectJson: '{"canvas":{"cards":[{"id":"a"}],"connections":[]}}', layout: [],
    definitions: [definitionFixture('a', '{"type":"input"}', '{"content":"before"}')], resolution: [resolved(['a'], 1)],
  }))
  return s
}
// Lock operations publish a carried source but provide no undo receipt.
{
  const s = session()
  const lock = '{"entries":[{"name":"science","url":"github:owner/repo","rev":"abcdef1234567890abcdef1234567890abcdef12","narHash":"sha256-example"}]}'
  let calls = 0
  s.operateProject = async (request: H.ProjectOperateRequestT) => {
    assert.equal(request.bodyType, H.ProjectOperationBody.ProjectUpdateNixPathLock)
    const body = request.body as H.ProjectUpdateNixPathLockT
    assert.equal(body.projectPath, calls === 0 ? null : 'externals/sub')
    assert.equal(body.name, calls === 0 ? 'science' : null)
    const path = (calls++ === 0 ? '' : 'externals/sub/') + 'tsugumi.lock'
    const delta = Object.assign(new R.DocumentDeltaT(), { baseRev: s.rev, rev: s.rev + 1n,
      sourcesUpsert: [Object.assign(new R.SourceInfoT(), { path, document: textBytes(lock) })] })
    return Object.assign(new R.SaveResultT(), { rev: delta.rev, delta })
  }
  await s.updateNixPathLock('', 'science')
  await s.updateNixPathLock('externals/sub/')
  assert.equal(serialize(s.nixPathLock(''), false), lock)
  assert.equal(serialize(s.nixPathLock('externals/sub/'), false), lock)
  assert.equal(s.undoStack.length, 0)
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
  const changed = Object.assign(new R.DocumentDeltaT(), { baseRev: 0n, rev: 1n, origin: 'agent',
    resolution: [resolved(['a'], 2)] })
  s.applyDelta(changed)
  const guards: (bigint | null)[] = []
  s.operateProject = async (request: H.ProjectOperateRequestT) => {
    assert.equal(request.bodyType, H.ProjectOperationBody.ProjectRewriteParamLiteral)
    const param = request.body as H.ProjectRewriteParamLiteralT
    guards.push(param.expectedRev)
    assert.deepEqual(param.card?.segments, ['a'])
    assert.equal(param.name, 'value')
    assert.equal(param.literal, '3')
    if (param.expectedRev !== s.rev) throw new RPCFailure({ kind: D.FailureKind.revision_conflict })
    return saved(s, request)
  }
  assert.equal(await edit.commitParam(s, 'a', 'value', '3', false), false)
  assert.equal(edit.conflictRev, 1n, edit.error)
  assert.equal(await edit.commitParam(s, 'a', 'value', '3', true), true)
  assert.deepEqual(guards, [0n, 1n])

  const draft = session()
  await draft.startDraftEdit('a')
  draft.inlineDraft.content = 'mine'
  draft.applyDelta(Object.assign(new R.DocumentDeltaT(), { baseRev: 0n, rev: 1n, origin: 'agent',
    definitionsUpsert: [definitionFixture('a', '{"type":"draft"}', '{"content":"agent","description":"unrelated"}')] }))
  const bases: string[] = []
  draft.operateProject = async (request: H.ProjectOperateRequestT) => {
    const json = (request.body as H.ProjectEditsT).edits[0].value as H.ProjectJSONEditT
    bases.push(String(JSON.parse(String(json.oldValue!.json))).split('\n').at(-1))
    if (bases.at(-1) !== 'agent') throw new RPCFailure({ kind: D.FailureKind.value_conflict })
    assert.match(JSON.parse(String(json.newValue!.json)), /description: unrelated/)
    return saved(draft, request)
  }
  await draft.finishDraftEdit()
  assert.equal(draft.inlineDraft.revision.conflictRev, 1n)
  assert.equal(draft.inlineDraft.content, 'mine')
  await draft.finishDraftEdit(true)
  assert.equal(draft.inlineDraft, undefined)
  assert.deepEqual(bases, ['before', 'agent'])
}

// A queued gesture uses the nested document from the preceding revision,
// whether the save reply or the notification delivers it first.
for (const echoFirst of [false, true]) {
  const s = session()
  const prefix = 'externals/sub/'
  s.confirmedDefinitions.set('sub', parse('{"type":"subgraph","external":"sub"}'))
  s.confirmedProject = parse('{"canvas":{"cards":[{"id":"sub"}],"connections":[]}}')
  const source = (file: string, documentJson: string) =>
    Object.assign(new R.SourceInfoT(), { path: prefix + file, document: textBytes(documentJson) })
  s.applySources([
    source('tsugumi.json', '{"canvas":{"cards":[{"id":"a"}],"connections":[]}}'),
    source('definitions/a/_card.json', '{"type":"input"}'),
  ], [])
  s.client.readFile = async () => { throw new Error('Unexpected extra file read') }
  let calls = 0
  s.operateProject = async (request: H.ProjectOperateRequestT) => {
    const param = request.body as H.ProjectRewriteParamLiteralT
    assert.deepEqual(param.card?.segments, ['sub', 'a'])
    assert.equal(param.expectedRev, s.rev)
    assert.equal(param.literal, ++calls === 1 ? '2' : '3')
    const result = saved(s, request, inverse('internal'), { resolution: [resolved(['sub', 'a'], calls === 1 ? 2 : 3)] })
    if (echoFirst) s.applyDelta(result.delta)
    return result
  }
  await s.setParam('sub/a', 'value', '2', s.rev)
  assert.equal(numText(s.resolution.get('sub/a').parameters[0].value), '2')
  await s.setParam('sub/a', 'value', '3', s.rev)
  assert.equal(calls, 2)
  assert.equal(numText(s.resolution.get('sub/a').parameters[0].value), '3')
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
  assert.equal(await edit.apply(s, x => x.setProjectField('name', 'mine')), false)
  assert.equal(edit.conflictRev, s.rev)
  assert.equal(edit.error, undefined)
  assert.equal(edit.applying, false)
}

// Canvas parameters and inline Drafts retain the conflict-time guard across a third delta.
for (const draft of [false, true]) {
  const s = session(), edit = new InlineParamEdit()
  edit.begin('1', s.rev)
  if (draft) { await s.startDraftEdit('a'); s.inlineDraft.content = 'mine' }
  const fields = (n: number) => ({ definitionsUpsert: [definitionFixture('a',
    '{"type":"input"}',
    JSON.stringify({ content: String(n), description: 'unrelated' }))], resolution: [resolved(['a'], n)] })
  const publish = (n: number) => s.applyDelta(Object.assign(new R.DocumentDeltaT(), { baseRev: s.rev, rev: s.rev + 1n, origin: 'agent', ...fields(n) }))
  publish(2)
  let actual = 2
  const guards: string[] = []
  s.operateProject = async (request: H.ProjectOperateRequestT) => {
    if (draft) {
      const field = (request.body as H.ProjectEditsT).edits[0].value as H.ProjectJSONEditT
      const before = parse(String(field.oldValue!.json))
      const guard = String(before).split('\n').at(-1)!
      guards.push(guard)
      if (guard !== String(actual)) throw new RPCFailure({ kind: D.FailureKind.value_conflict })
      assert.match(JSON.parse(String(field.newValue!.json)), /description: unrelated/)
    } else {
      const param = request.body as H.ProjectRewriteParamLiteralT
      guards.push(String(param.expectedRev))
      if (param.expectedRev !== s.rev) throw new RPCFailure({ kind: D.FailureKind.revision_conflict })
    }
    return saved(s, request, inverse('inline'), draft ? { definitionsUpsert: [definitionFixture('a', serialize(s.cardDefinition('a')), '{"content":"mine","description":"unrelated"}')] } : fields(3))
  }
  const save = async (overwrite: boolean) => {
    if (draft) { await s.finishDraftEdit(overwrite); return !s.inlineDraft }
    return edit.commitParam(s, 'a', 'value', '3', overwrite)
  }
  assert.equal(await save(false), false)
  actual = 4; publish(actual)
  assert.equal(await save(true), false)
  assert.equal(guards.at(-1), draft ? '2' : '1')
  assert.equal(await save(true), true)
  assert.equal(guards.at(-1), draft ? '4' : '2')
  assert.equal(draft ? get(s.cardMeta('a'), 'content') : numText(s.resolution.get('a').parameters[0].value), draft ? 'mine' : '3')
}

// Each entity editor uses the same field working-copy path as its UI closure.
// A third delta after the notice must fail, then the newly shown value can be overwritten.
for (const kind of ['note', 'group', 'comment'] as const) {
  for (const third of [false, true]) {
    const s = session(), edit = new InlineParamEdit()
    const file = kind === 'note' || kind === 'group' ? 'layout.bin' : 'definitions/a/_comments.json'
    const field = kind === 'note' ? 'notes' : kind === 'group' ? 'groups' : 'comments'
    const selector = [field, obj([['id', 'item']])]
    const value = (text: string): J => obj([['id', 'item'], [kind === 'group' ? 'name' : 'text', text]])
    const document = (text: string) => obj([[field, [value(text)]]])
    const fields = (text: string) => {
      const encoded = serialize(document(text))
      if (file !== 'layout.bin') return { definitionsUpsert: [definitionFixture('a', serialize(s.cardDefinition('a')), encoded)] }
      const { bytes, notes } = layoutFixture(encoded)
      const sourcesUpsert = [...notes].map(([id, contents]) => Object.assign(new R.SourceInfoT(), {
        path: `notes/${id}.md`, document: textBytes(contents),
      }))
      return { layout: bytes, layoutChanged: true, sourcesUpsert }
    }
    const publish = (text: string) => s.applyDelta(Object.assign(new R.DocumentDeltaT(), {
      baseRev: s.rev, rev: s.rev + 1n, origin: 'agent', ...fields(text),
    }))
    publish('before')
    edit.beginField('before', s, file, selector, value('before'))
    publish('agent')
    let actual = 'agent'
    const wanted = 'mine'
    const guards: string[] = []
    s.operateProject = async (request: H.ProjectOperateRequestT) => {
      const field = (request.body as H.ProjectEditsT).edits[0].value as H.ProjectFieldEditT
      const before = String(field.oldValue!.json)
      guards.push(before)
      if (before !== serialize(value(actual))) throw new RPCFailure({ kind: D.FailureKind.value_conflict })
      assert.equal(field.newValue!.json, serialize(value(wanted)), kind)
      return saved(s, request, inverse('entity'), fields(wanted))
    }
    const save = (overwrite: boolean) => edit.commitField(s, wanted, overwrite, () => value(wanted))
    assert.equal(await save(false), false, kind)
    if (third) {
      actual = 'third'; publish(actual)
      assert.equal(await save(true), false, kind + ': third writer must be refused')
      assert.equal(guards.at(-1), serialize(value('agent')))
    }
    assert.equal(await save(true), true, kind + ': overwrite must save')
    assert.equal(guards.at(-1), serialize(value(actual)))
    assert.equal(file === 'layout.bin' ? serialize(s.layout) : serialize(s.cardMeta('a')), serialize(document('mine')))
  }
}

// Card settings include both JSON and source guards. Both are frozen at conflict time.
const { CardEditorModel } = await import('../src/ui/cardSettings')
for (const source of [false, true]) {
  for (const third of [false, true]) {
    const s = session(), file = 'definitions/a/main.py'
    s.sources.set(file, { hash: 'before', size: 1n })
    const editor = new CardEditorModel(s, 'a')
    if (source) { s.client.readFile = async () => new TextEncoder().encode('before'); await editor.text(file); editor.setText(file, 'mine') }
    else editor.setSetting('symbol', 'mine')
    const publish = (text: string) => s.applyDelta(Object.assign(new R.DocumentDeltaT(), {
      baseRev: s.rev, rev: s.rev + 1n, origin: 'agent',
      definitionsUpsert: [definitionFixture('a', serialize(obj([['type', 'input'], ['symbol', text]])), serialize(s.cardMeta('a')))],
      sourcesUpsert: [Object.assign(new R.SourceInfoT(), { path: file, hash: text, sizeBytes: 1n })],
    }))
    publish('agent')
    let actual = 'agent'
    const guards: string[] = []
    s.operateProject = async (request: H.ProjectOperateRequestT) => {
      const edits = (request.body as H.ProjectEditsT).edits
      const put = edits.find(e => e.valueType === H.ProjectEditValue.ProjectFilePut)?.value as H.ProjectFilePutT | undefined
      const card = edits.find(e => (e.value as H.ProjectJSONEditT).filePath === 'definitions/a/_card.json')!.value as H.ProjectJSONEditT
      const guard = source ? String(put!.expectedHash) : String(get(parse(String(card.oldValue!.json)), 'symbol') ?? 'before')
      guards.push(guard)
      if (guard !== actual) throw new RPCFailure({ kind: source ? D.FailureKind.source_changed : D.FailureKind.value_conflict })
      if (source) assert.equal(new TextDecoder().decode(new Uint8Array(put!.bytes)), 'mine')
      else assert.equal(get(parse(String(card.newValue!.json)), 'symbol'), 'mine')
      return saved(s, request, inverse('settings'), { definitionsUpsert: [definitionFixture('a', String(card.newValue!.json), serialize(s.cardMeta('a')))],
        sourcesUpsert: [Object.assign(new R.SourceInfoT(), { path: file, hash: 'mine', sizeBytes: 4n })] })
    }
    assert.equal(await editor.apply(), false)
    if (third) { actual = 'third'; publish(actual); assert.equal(await editor.apply(true), false); assert.equal(guards.at(-1), 'agent') }
    assert.equal(await editor.apply(true), true, editor.revisionEdit.error ?? `settings source=${source} third=${third}`)
    assert.equal(guards.at(-1), actual)
    assert.equal(source ? s.sources.get(file).hash : get(s.cardDefinition('a'), 'symbol'), 'mine')
  }
}

// Raw definitions replace whole documents using the revision shown by the notice.
for (const third of [false, true]) {
  const s = session(), edit = new RevisionEdit()
  edit.reload(s.rev)
  const publish = () => s.applyDelta(Object.assign(new R.DocumentDeltaT(), { baseRev: s.rev, rev: s.rev + 1n, origin: 'agent' }))
  publish()
  const revisions: bigint[] = []
  s.operateProject = async (request: H.ProjectOperateRequestT) => {
    const body = request.body as H.ProjectReplaceDocumentsT
    revisions.push(body.expectedRev)
    if (body.expectedRev !== s.rev) throw new RPCFailure({ kind: D.FailureKind.revision_conflict })
    return saved(s, request, inverse('raw'), { definitionsUpsert: [definitionFixture('a', '{"type":"draft"}', '{"content":"mine"}')] })
  }
  const save = (overwrite: boolean) => edit.replaceDocuments(s, [['definitions/a/_card.json', parse('{"type":"draft"}')], ['definitions/a/README.md', 'mine']], overwrite)
  assert.equal(await save(false), false)
  assert.equal(edit.conflictRev, 1n)
  assert.equal(edit.error, undefined)
  if (third) { publish(); assert.equal(await save(true), false); assert.equal(revisions.at(-1), 1n) }
  assert.equal(await save(true), true)
  assert.equal(get(s.cardMeta('a'), 'content'), 'mine')
}

console.log('reviewOperations: passed')
