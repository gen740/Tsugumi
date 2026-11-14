import assert from 'node:assert/strict'
import { Builder, ByteBuffer } from 'flatbuffers'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { clone, get, parse, str } from '../src/shared/json'
import { D, R, RPCClient, RPCFailure, RequestPayload, s as text } from '../src/shared/rpc'
import { ProjectSession } from '../src/model/session'
import { CardEditorModel } from '../src/ui/cardSettings'
import { InlineParamEdit } from '../src/ui/revisionEdit'
import { PermissionRequests } from '../src/ui/chat'
import { runMark } from '../src/model/runMark'

function deferred<T>() {
  let resolve!: (v: T) => void
  const promise = new Promise<T>(r => { resolve = r })
  return { promise, resolve }
}
function session() {
  const s = new ProjectSession('/p', 'k', 'p')
  s.connected = true
  s.scheduleSave = () => {}
  s.project = parse('{"name":"base","canvas":{"cards":[{"id":"a"},{"id":"b"},{"id":"c"}],"connections":[{"from":{"card":"a","port":"out"},"to":{"card":"b","port":"in"}}]}}')
  s.confirmedProject = clone(s.project)
  s.layout = s.confirmedLayout = parse('{}')
  for (const id of ['a', 'b', 'c']) {
    s.confirmedDefinitions.set(id, parse('{"template":"draft","inputs":[{"name":"in"}],"symbol":"base"}'))
    s.confirmedDefinitionMeta.set(id, parse('{"outputs":[{"name":"out"}]}'))
  }
  return s
}

// A fork allocation suspends connect. Neither missing end, a removed port,
// nor losing editability can leave a dangling wire in the draft.
for (const change of ['source', 'target', 'source-port', 'target-port', 'offline', 'running']) {
  const s = session(), id = deferred<string>()
  s.cardIdProvider = () => id.promise
  const connecting = s.connect(['a', 'out'], ['c', 'in'], [])
  if (change === 'source' || change === 'target') s.deleteCards(new Set([change === 'source' ? 'a' : 'c']), [])
  if (change === 'source-port') s.updateMeta('a', m => m.set('outputs', []))
  if (change === 'target-port') s.updateCard('c', c => c.set('inputs', []))
  if (change === 'offline') s.connected = false
  if (change === 'running') s.runStatus = Object.assign(new R.RunStatusT(), { state: D.RunState.running })
  const before = clone(s.project)
  id.resolve('fork-new')
  await connecting
  assert.deepEqual(s.project, before, change)
  assert.equal(s.instance('fork-new'), undefined)
}

// Only the submitted settings/source become saved; typing and file operations
// after Send remain in the working copy and are included in the next apply.
{
  const s = session(), pending = deferred<R.SaveResultT>(), sent = deferred<void>()
  const p = 'definitions/a/source.txt'
  s.sources.set(p, { hash: 'base', size: 4n })
  let changes: R.DocumentChangeT[] = []
  s.client = {
    readFile: async () => new TextEncoder().encode('base'),
    save: async (_rev: bigint, _op: string, _origin: string, value: R.DocumentChangeT[]) => {
      changes = value; sent.resolve(); return pending.promise
    },
  } as never
  const m = new CardEditorModel(s, 'a')
  await m.text(p)
  m.setText(p, 'sent')
  m.setSetting('symbol', 'sent')
  const saving = m.apply()
  await sent.promise
  m.setText(p, 'later')
  m.setSetting('symbol', 'later')
  m.createFile('later.txt', 'new')
  const delta = Object.assign(new R.DocumentDeltaT(), { baseRev: 0n, rev: 1n, executionRev: 0n })
  for (const c of changes) if (c.valueType === R.DocumentChangeValue.PutDefinition) {
    delta.definitionsUpsert.push(Object.assign(new R.DefinitionT(), c.value))
  }
  delta.sourcesUpsert.push(Object.assign(new R.SourceInfoT(), { path: p, hash: 'sent', sizeBytes: 4n }))
  pending.resolve(Object.assign(new R.SaveResultT(), { rev: 1n, delta }))
  assert.equal(await saving, true)
  assert.equal(m.loadedText(p), 'later')
  assert.equal(str(get(m.definition, 'symbol')), 'later')
  assert.equal(m.unapplied, true)
  assert.equal(m.isEdited(p), true)
  assert.ok(m.created.has('definitions/a/later.txt'))
  assert.equal(str(get(s.cardDefinition('a'), 'symbol')), 'sent')
}

// Blur without typing cannot replace the Agent's newer value. A changed value
// retains the opening revision and is rejected outside the autosaved draft.
{
  const s = session(), edit = new InlineParamEdit()
  let writes = 0
  s.applyEdit = async () => { writes++; throw new RPCFailure({ kind: D.FailureKind.revision_conflict }) }
  edit.begin('1', 0n)
  s.rev = 1n
  assert.equal(await edit.commit(s, '1', false, () => assert.fail()), true)
  assert.equal(writes, 0)
  assert.equal(await edit.commit(s, '3', false, () => assert.fail()), false)
  assert.equal(edit.baseRev, 0n)
  assert.equal(edit.conflictRev, 1n)
  assert.equal(s.dirty, false)
}

// The actual reload RPC boundary preserves every repeated permission argument,
// presents them, then re-sends the approved items with a new operation id.
for (const allow of [true, false]) {
  const s = session(), calls: R.ProjectReloadRequestT[] = []
  const asked = ['network a', 'sandbox_off b']
  s.client = new RPCClient({ close() {}, async call(frame) {
    const message = R.Message.getRootAsMessage(new ByteBuffer(frame)).unpack()
    const request = message.payload as R.RequestT
    assert.equal(request.valueType, RequestPayload.ProjectReloadRequest)
    const r = request.value as R.ProjectReloadRequestT
    calls.push(r)
    let reply: R.ReplyT
    if (calls.length === 1) reply = new R.ReplyT(R.ReplyPayload.Failure, Object.assign(new D.FailureT(), {
      kind: D.FailureKind.approval_required,
      args: asked.map(value => new D.KeyValueT('permission', value)),
    }))
    else {
      assert.deepEqual(r.approvedPermissions.map(text), asked)
      assert.notEqual(r.opId, calls[0].opId)
      assert.equal(r.baseRev, calls[0].baseRev)
      reply = new R.ReplyT(R.ReplyPayload.SaveResult, new R.SaveResultT())
    }
    const builder = new Builder(512)
    builder.finish(new R.MessageT(message.requestId, R.Payload.Reply, reply).pack(builder), 'TSG1')
    return builder.asUint8Array().slice()
  } })
  await s.reloadExternal()
  assert.deepEqual(s.reloadApproval?.permissions, asked)
  const html = renderToStaticMarkup(<PermissionRequests asked={s.reloadApproval!.permissions} />)
  assert.match(html, /a:/); assert.match(html, /b:/)
  assert.doesNotMatch(html, /今後は確認しない/)
  await s.answerReloadApproval(allow)
  assert.equal(calls.length, allow ? 2 : 1)
  assert.equal(s.reloadApproval, undefined)
  assert.equal(s.saveError, undefined)
}
assert.equal(runMark(new R.RunStatusT()), undefined)
{
  const status = new R.RunStatusT()
  status.partialRun = true
  assert.equal(runMark(status), '部分実行')
  status.partialRun = false; status.usesPartialResult = true
  assert.equal(runMark(status), '部分実行の結果を使用')
  status.partialRun = true
  const summary = new R.RunSummaryT(); summary.status = status
  assert.equal(runMark(summary), '部分実行 · 部分実行の結果を使用')
}

// An outside editor may add another widening while approval is on screen.
// Keep the previously approved items when the core asks for the new one.
{
  const s = session(), approvals: string[][] = []
  s.client = { reload: async (_base: bigint, _op: string, _origin: string, approved: string[]) => {
    approvals.push(approved)
    const missing = ['network a', 'sandbox_off b'].filter(p => !approved.includes(p))
    const next = approvals.length === 1 ? ['network a'] : missing
    if (next.length) throw new RPCFailure({ kind: D.FailureKind.approval_required, args: next.map(p => new D.KeyValueT('permission', p)) })
    return new R.SaveResultT()
  } } as never
  await s.reloadExternal()
  await s.answerReloadApproval(true)
  assert.deepEqual(s.reloadApproval?.permissions, ['network a', 'sandbox_off b'])
  await s.answerReloadApproval(true)
  assert.deepEqual(approvals, [[], ['network a'], ['network a', 'sandbox_off b']])
  assert.equal(s.reloadApproval, undefined)
}

// The canvas mutation stays inside RevisionEdit's transaction. A rejected RPC
// must not launch the ordinary canvas autosave or add a second undo step.
{
  const s = session(), edit = new InlineParamEdit()
  s.confirmedDefinitions.set('a', parse('{"template":"input","params":[{"name":"value","type":"uint64","data":1}]}'))
  let autosaves = 0, requests = 0
  s.scheduleSave = () => { autosaves++ }
  s.client = { save: async () => { requests++; throw new RPCFailure({ kind: D.FailureKind.revision_conflict }) } } as never
  edit.begin('1', s.rev)
  assert.equal(await edit.commit(s, '3', false, x => x.updateParam('a', 'value', parse('3'))), false)
  assert.equal(autosaves, 0)
  assert.equal(requests, 1)
  assert.equal(s.dirty, false)
  assert.equal(s.undoStack.length, 0)
}
console.log('owner UI review: delayed connect, in-flight edits, inline revision, reload approval passed')
