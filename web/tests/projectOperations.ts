// Run this file alone with esbuild; the transport is advanced explicitly.
import assert from 'node:assert/strict'
import { Builder, ByteBuffer } from 'flatbuffers'
import * as H from '../src/gen/tsugumi/host'
import { R } from '../src/shared/rpc'
import { parse, get, str, serialize } from '../src/shared/json'
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
  }), true)
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
console.log('projectOperations: passed')
