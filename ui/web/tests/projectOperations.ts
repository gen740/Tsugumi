// Run this file alone with esbuild; the transport is advanced explicitly.
import assert from 'node:assert/strict'
import { Builder, ByteBuffer } from 'flatbuffers'
import * as H from '../shared/gen/tsugumi/host'
import { R } from '../src/wire/rpc'
import { parse, get, str, serialize } from '../shared/json'
import { DocumentRevisions } from '../src/model/documentRevisions'
import { encodeRequest, decodeReply } from '../server/serviceWire'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')

function deferred<T>() {
  let resolve!: (value: T) => void
  const promise = new Promise<T>(r => { resolve = r })
  return { promise, resolve }
}
function delta(baseRev: bigint, opId: string, name: string) {
  return Object.assign(new R.DocumentDeltaT(), { baseRev, rev: baseRev + 1n, opId, origin: 'ui',
    projectJson: `{"name":"${name}","canvas":{"cards":[],"connections":[]}}` })
}
function result(d: R.DocumentDeltaT) {
  const inverse = new H.ProjectEditsT([new H.ProjectEditT(H.ProjectEditValue.ProjectFieldEdit,
    new H.ProjectFieldEditT('tsugumi.json', [new H.ProjectSelectorT('name')],
      new H.ProjectValueT(true, '"new"'), new H.ProjectValueT(true, '"old"')))])
  const builder = new Builder(); builder.finish(inverse.pack(builder))
  return Object.assign(new R.SaveResultT(), { delta: d, rev: d.rev, operationData: Array.from(builder.asUint8Array()) })
}

// Both orderings use the real session operation completion path, immediately
// after opening. There are no save bases, sleeps, timers or document merges.
for (const echoFirst of [false, true]) {
  const s = new ProjectSession('/test', 'test', 'test') as any
  s.projectId = 'p'; s.client = {}; s.connected = true
  s.refreshExternals = () => {}; s.refreshDerivedLayout = () => {}
  s.apply(Object.assign(new R.ProjectSnapshotT(), {
    rev: 0n, projectJson: '{"name":"old","canvas":{"cards":[],"connections":[]}}', layoutJson: '{}',
  }))
  const entered = deferred<H.ProjectOperateRequestT>(), reply = deferred<R.SaveResultT>()
  s.operateProject = (request: H.ProjectOperateRequestT) => { entered.resolve(request); return reply.promise }
  const action = s.operateFields([s.fieldEdit('tsugumi.json', ['name'], 'old', 'new')])
  const request = await entered.promise
  const d = delta(0n, String(request.opId), 'new')
  if (echoFirst) s.applyDelta(d)
  reply.resolve(result(d))
  await action
  if (!echoFirst) s.applyDelta(d)
  s.applyDelta(d) // Duplicate echo.
  assert.equal(s.rev, 1n)
  assert.equal(str(get(s.project, 'name')), 'new')
  assert.equal(s.undoStack.length, 1)

  // Another writer's update is applied between our two operations. The
  // second operation has no global base revision that could erase it.
  s.applyDelta(delta(1n, 'other', 'other'))
  const nextEntered = deferred<H.ProjectOperateRequestT>(), nextReply = deferred<R.SaveResultT>()
  s.operateProject = (r: H.ProjectOperateRequestT) => { nextEntered.resolve(r); return nextReply.promise }
  const second = s.operateFields([s.fieldEdit('tsugumi.json', ['name'], 'other', 'last')])
  const next = await nextEntered.promise
  assert.notEqual(next.opId, request.opId)
  assert.equal((next as any).baseRev, undefined)
  nextReply.resolve(result(delta(2n, String(next.opId), 'last')))
  await second
  assert.equal(s.rev, 3n)
  assert.equal(str(get(s.project, 'name')), 'last')
}

// A reply may arrive ahead of another writer's notification. Completion waits
// for application, without requiring that our notification arrive at all.
{
  const revisions = new DocumentRevisions(4n), seen: bigint[] = []
  const apply = (d: R.DocumentDeltaT) => seen.push(d.rev!)
  revisions.receive(delta(5n, 'mine', 'mine'), apply)
  const done = revisions.applied(6n)
  assert.deepEqual(seen, [])
  revisions.receive(delta(4n, 'other', 'other'), apply)
  await done
  revisions.receive(delta(5n, 'mine', 'mine'), apply)
  assert.deepEqual(seen, [5n, 6n])
}

// HTTP and Host adapters carry typed bytes, including binary inverse payloads
// and revisions greater than JavaScript's exact integer range.
{
  const input = new H.ProjectOperateRequestT('p', 'an-operation-id', H.ProjectOperationBody.ProjectLayout,
    new H.ProjectLayoutT('', H.ProjectLayoutAction.Untangle))
  const packed = new Builder(); packed.finish(input.pack(packed))
  const wire = encodeRequest('ProjectOperateRequest', { bytes: Array.from(packed.asUint8Array()) }, 7n, true)
  const request = H.Message.getRootAsMessage(new ByteBuffer(wire)).unpack().payload as H.RequestT
  assert.deepEqual(request.value, input)
  const saved = result(delta(1n << 60n, 'mine', 'new'))
  const envelope = new H.MessageT(7n, H.Payload.Reply,
    new H.ReplyT(H.ReplyPayload.ProjectOperationResult, new H.ProjectOperationResultT(new R.ReplyT(R.ReplyPayload.SaveResult, saved))))
  const builder = new Builder(); H.Message.finishMessageBuffer(builder, envelope.pack(builder))
  const dto = decodeReply(builder.asUint8Array(), true).value
  const restored = H.ProjectOperationResult.getRootAsProjectOperationResult(new ByteBuffer(new Uint8Array(dto.bytes))).unpack()
  assert.equal((restored.reply!.value as R.SaveResultT).rev, saved.rev)
  assert.deepEqual((restored.reply!.value as R.SaveResultT).operationData, saved.operationData)
}

// Canvas setters enqueue synchronously. A second gesture builds its selected
// values after the first reply has entered the confirmed document.
{
  const s = new ProjectSession('/test', 'queued-settings', 'test') as any
  s.projectId = 'p'; s.client = {}; s.connected = true
  s.refreshExternals = () => {}; s.refreshDerivedLayout = () => {}
  s.apply(Object.assign(new R.ProjectSnapshotT(), {
    rev: 0n, projectJson: '{"name":"old","canvas":{"cards":[],"connections":[]}}', layoutJson: '{"flow":"topToBottom"}',
  }))
  const first = deferred<H.ProjectOperateRequestT>(), second = deferred<H.ProjectOperateRequestT>()
  const firstReply = deferred<R.SaveResultT>(), secondReply = deferred<R.SaveResultT>()
  let calls = 0
  s.operateProject = (request: H.ProjectOperateRequestT) => {
    if (calls++ === 0) { first.resolve(request); return firstReply.promise }
    second.resolve(request); return secondReply.promise
  }
  s.setFlow('leftToRight'); s.setFlow('topToBottom')
  const complete = s.operationTask
  const one = await first.promise
  const field = (r: H.ProjectOperateRequestT) => (r.body as H.ProjectEditsT).edits[0].value as H.ProjectFieldEditT
  assert.equal(JSON.parse(String(field(one).oldValue!.json)), 'topToBottom')
  const d1 = delta(0n, String(one.opId), 'old'); d1.layoutJson = '{"flow":"leftToRight"}'
  firstReply.resolve(result(d1))
  const two = await second.promise
  assert.equal(JSON.parse(String(field(two).oldValue!.json)), 'leftToRight')
  assert.equal(JSON.parse(String(field(two).newValue!.json)), 'topToBottom')
  const d2 = delta(1n, String(two.opId), 'old'); d2.layoutJson = '{"flow":"topToBottom"}'
  secondReply.resolve(result(d2))
  await complete
  assert.equal(s.rev, 2n)
  assert.equal(s.flow, 'topToBottom')
  assert.equal(calls, 2)
}

// Transformations and moves send intent without reading or assembling source
// bytes in the browser. The host tests exercise binary/nested closure and Undo.
for (const action of ['create', 'import', 'copy', 'duplicate', 'convert', 'move'] as const) {
  const s = new ProjectSession('/test', 'intent-' + action, 'test') as any
  s.projectId = 'p'; s.client = { readFile: () => { throw new Error('client source read') } }; s.connected = true
  s.refreshExternals = () => {}; s.refreshDerivedLayout = () => {}
  s.cardIdProvider = async () => 'new-card'
  s.apply(Object.assign(new R.ProjectSnapshotT(), {
    rev: 0n, projectJson: '{"canvas":{"cards":[{"id":"a"}],"connections":[]}}', layoutJson: '{"cards":{"a":{"x":0,"y":0}}}',
    definitions: [new R.DefinitionT('a', '{"template":"draft"}')],
  }))
  const requests: H.ProjectOperateRequestT[] = []
  s.operateProject = async (request: H.ProjectOperateRequestT) => {
    requests.push(request)
    const d = Object.assign(new R.DocumentDeltaT(), { baseRev: 0n, rev: 1n, opId: request.opId, origin: 'ui' })
    return result(d)
  }
  const at = { x: 120, y: 240 }
  if (action === 'create') await s.addCard('draft', [], at)
  else if (action === 'import') await s.importProject('/projects/source', [], at)
  else if (action === 'copy') await s.placeDefinitionCopy('', 'a', [], at)
  else if (action === 'duplicate') await s.duplicate(new Set(['a']), [])
  else if (action === 'convert') await s.makeSubgraph(new Set(['a']), [])
  else await s.moveSelection([], new Set(['a']), new Set(), new Set(), 100, 0)
  assert.equal(requests.length, 1, action)
  const types = { create: H.ProjectOperationBody.ProjectCreateCard, import: H.ProjectOperationBody.ProjectImport,
    copy: H.ProjectOperationBody.ProjectCopy, duplicate: H.ProjectOperationBody.ProjectCopy,
    convert: H.ProjectOperationBody.ProjectMakeSubgraph, move: H.ProjectOperationBody.ProjectMove }
  assert.equal(requests[0].bodyType, types[action])
  assert.equal(s.rev, 1n)
  assert.equal(s.dirty, false)
}

// Reveal and open the inline editor only with the new Draft's placement.
{
  const { CanvasController } = await import('../src/model/canvas')
  const { WorkflowTab } = await import('../src/model/workspace')
  const s = new ProjectSession('/test', 'draft-placement', 'test')
  const fake = s as any
  s.projectId = 'p'; fake.client = {}; s.connected = true
  fake.refreshExternals = () => {}
  s.saveViewSoon = () => {}
  fake.cardIdProvider = async () => 'draft'
  const placement = deferred<string>()
  s.layoutHost = { ...s.layoutHost, complete: () => placement.promise }
  fake.operateProject = async (request: H.ProjectOperateRequestT) => {
    const d = delta(0n, String(request.opId), 'draft')
    d.projectJson = '{"canvas":{"cards":[{"id":"draft"}],"connections":[]}}'
    return result(d)
  }
  const canvas = new CanvasController(s, new WorkflowTab([]))
  canvas.size = { width: 800, height: 600 }
  const draft = await canvas.addDraft()
  assert.ok(draft)
  const writing = canvas.writeDraft(draft)
  assert.equal(Boolean(s.inlineDraft), false)
  placement.resolve('{"cards":{"draft":{"x":4000,"y":4000}}}')
  await writing
  const rect = canvas.scene().cardRect(draft)
  const a = canvas.screen(rect), b = canvas.screen({ x: rect.x + rect.width, y: rect.y + rect.height })
  assert.ok(a.x >= 0 && a.y >= 0 && b.x <= canvas.size.width && b.y <= canvas.size.height)
  assert.equal(s.inlineDraft?.card, draft)
}

// Undo requested before an action's reply reverses that action once applied.
{
  const s = new ProjectSession('/test', 'pending-undo', 'test') as any
  s.projectId = 'p'; s.client = {}; s.connected = true
  s.refreshExternals = () => {}; s.refreshDerivedLayout = () => {}
  s.apply(Object.assign(new R.ProjectSnapshotT(), {
    rev: 0n, projectJson: '{"name":"old","canvas":{"cards":[],"connections":[]}}', layoutJson: '{}',
  }))
  const entered = deferred<H.ProjectOperateRequestT>(), undoEntered = deferred<H.ProjectOperateRequestT>()
  const reply = deferred<R.SaveResultT>(), undoReply = deferred<R.SaveResultT>()
  let calls = 0
  s.operateProject = (request: H.ProjectOperateRequestT) => {
    if (calls++ === 0) { entered.resolve(request); return reply.promise }
    undoEntered.resolve(request); return undoReply.promise
  }
  s.setFlow('leftToRight')
  const undo = s.undo()
  const request = await entered.promise
  assert.equal(s.undoStack.length, 0)
  reply.resolve(result(delta(0n, String(request.opId), 'new')))
  const inverse = await undoEntered.promise
  assert.equal(inverse.bodyType, H.ProjectOperationBody.ProjectEdits)
  undoReply.resolve(result(delta(1n, String(inverse.opId), 'old')))
  await undo
  assert.equal(calls, 2)
  assert.equal(s.rev, 2n)
  assert.equal(s.undoStack.length, 0)
  assert.equal(s.redoStack.length, 1)
}

// The remaining editor/form/selection paths share the operation transport.
{
  const { CardEditorModel } = await import('../src/ui/cardSettings')
  const { RevisionEdit } = await import('../src/ui/revisionEdit')
  const s = new ProjectSession('/test', 'cutover', 'test') as any
  s.projectId = 'p'; s.client = {}; s.connected = true
  s.refreshExternals = () => {}; s.refreshDerivedLayout = () => {}
  s.apply(Object.assign(new R.ProjectSnapshotT(), {
    rev: 0n, projectJson: '{"name":"old","canvas":{"cards":[{"id":"a"}],"connections":[]}}', layoutJson: '{}',
    definitions: [new R.DefinitionT('a', '{"template":"draft"}', '{"description":"old"}')],
  }))
  const requests: H.ProjectOperateRequestT[] = []
  s.operateProject = async (request: H.ProjectOperateRequestT) => {
    requests.push(request)
    const d = delta(s.rev, String(request.opId), 'saved')
    return result(d)
  }
  const editor = new CardEditorModel(s, 'a')
  editor.setSetting('symbol', 'star')
  editor.createFile('main.py', 'print(1)')
  assert.equal(await editor.apply(), true)
  assert.equal(requests.length, 1)
  const edits = (requests[0].body as H.ProjectEditsT).edits
  assert.ok(edits.some(e => e.valueType === H.ProjectEditValue.ProjectJSONEdit))
  assert.ok(edits.some(e => e.valueType === H.ProjectEditValue.ProjectFilePut))
  assert.equal((edits.find(e => e.valueType === H.ProjectEditValue.ProjectFilePut)!.value as H.ProjectFilePutT).expectMissing, true)
  await s.applyMutation(() => {})
  assert.equal(requests.length, 1, 'an unchanged gesture has no operation')
  const form = new RevisionEdit()
  form.reload(0n) // A form has no global stale-revision gate.
  assert.equal(await form.apply(s, x => x.setProjectField('name', 'form')), true)
  assert.equal(requests.length, 2)
  await s.replaceDocuments(0n, [['tsugumi.json', parse('{"name":"raw"}')]])
  assert.equal(requests[2].bodyType, H.ProjectOperationBody.ProjectReplaceDocuments)
  assert.equal((requests[2].body as H.ProjectReplaceDocumentsT).expectedRev, 0n)
  s.deleteSelection(new Set(['a']), new Set(['note']), new Set(['frame']), [])
  await s.save()
  assert.equal(requests[3].bodyType, H.ProjectOperationBody.ProjectSelection)
  const selection = requests[3].body as H.ProjectSelectionT
  assert.deepEqual(selection.cards, ['a']); assert.deepEqual(selection.notes, ['note']); assert.deepEqual(selection.frames, ['frame'])
  assert.equal(selection.action, H.ProjectSelectionAction.Delete)
  await s.reloadExternal()
  assert.equal(requests[4].bodyType, H.ProjectOperationBody.ProjectReload)
}

// Presentation fixture traffic goes through the production host executor too.
{
  const { cardIds } = await import('./cardIds')
  const s = cardIds(new ProjectSession('/test', 'fixture-smoke', 'test'))
  s.project = parse('{"name":"fixture","canvas":{"cards":[],"connections":[]}}')
  s.confirmedProject = parse(serialize(s.project))
  const card = await s.addCard('draft', [], { x: 10, y: 20 })
  const binary = [0, 255, 128, 195, 40]
  const dir = s.definitionDir(card)!
  await s.operateFields([new H.ProjectEditT(H.ProjectEditValue.ProjectFilePut,
    new H.ProjectFilePutT(dir + 'data.bin', null, true, binary))])
  const { CardEditorModel } = await import('../src/ui/cardSettings')
  const editor = new CardEditorModel(s, card)
  const client = (s as any).client, read = client.readFile
  let reads = 0
  client.readFile = async (...args: unknown[]) => { reads++; return read(...args) }
  assert.equal(await editor.rename(dir + 'data.bin', 'renamed.bin'), true)
  assert.equal(reads, 0, 'rename sends byte-preserving intent without a source read')
  assert.equal(await editor.apply(), true)
  assert.deepEqual([...await client.readFile(dir + 'renamed.bin', s.rev)], binary)
  await editor.text(dir + 'renamed.bin')
  const beforeRename = reads
  assert.equal(await editor.rename(dir + 'renamed.bin', 'again.bin'), true)
  assert.equal(reads, beforeRename)
  assert.equal(await editor.apply(), true)
  assert.deepEqual([...await client.readFile(dir + 'again.bin', s.rev)], binary)

  await s.addComment(card, 'one operation')
  assert.equal(s.comments(card)[0].text, 'one operation')
  await s.undo()
  assert.deepEqual(s.comments(card), [])
  s.deleteCards(new Set([card]), [])
  await s.save()
  assert.equal(s.cardDefinition(card), undefined)
}
console.log('projectOperations: passed')
