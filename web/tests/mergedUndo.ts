// The reviewer's ordering: open name editor, notice another writer's group,
// merge the rename, notice deletion of that group, then undo the rename.
import assert from 'node:assert/strict'
import * as fb from 'flatbuffers'
import * as H from '../src/gen/tsugumi/host'
import { R, RPCClient } from '../src/shared/rpc'
import { ProjectSession } from '../src/model/session'
import { RevisionEdit } from '../src/ui/revisionEdit'
import { parse, clone, get, str } from '../src/shared/json'
import { hostDocumentReply } from './hostDocument'
import { encodeRequest, decodeReply } from '../server/serviceWire'

const realFetch = globalThis.fetch
try {
  const session = new ProjectSession('/undo', 'undo', 'undo')
  session.projectId = 'undo'
  session.connected = true
  session.scheduleSave = () => {}
  session.project = parse('{"name":"base","canvas":{"cards":[{"id":"a"}],"connections":[]}}')
  session.confirmedProject = clone(session.project)
  session.layout = parse('{"cards":{"a":{"x":0,"y":0}},"groups":[]}'); session.confirmedLayout = clone(session.layout)
  session.confirmedDefinitions.set('a', parse('{"template":"draft"}'))
  session.client = new RPCClient({ call: async () => { throw new Error('Save through Host') }, close() {} })
  session.prepareMerge = async (baseJson, oursJson, theirsJson) => hostDocumentReply({ baseJson, oursJson, theirsJson })
  let revision = 0n
  let files: Record<string, unknown> = { 'tsugumi.json': JSON.parse('{"name":"base","canvas":{"cards":[{"id":"a"}],"connections":[]}}'), 'layout.json': { cards: { a: { x: 0, y: 0 } }, groups: [] }, 'definitions/a/_card.json': { template: 'draft' } }
  const notice = (layout: unknown) => {
    files['layout.json'] = layout
    const delta = Object.assign(new R.DocumentDeltaT(), { baseRev: revision, rev: ++revision, layoutJson: JSON.stringify(layout) })
    ;(session as unknown as { applyDelta(d: R.DocumentDeltaT): void }).applyDelta(delta)
  }
  globalThis.fetch = (async (url, options) => {
    assert.equal(url, '/api/document/save')
    const encoded = encodeRequest('UISaveRequest', JSON.parse(String(options?.body)), 1n, true)
    const host = H.Message.getRootAsMessage(new fb.ByteBuffer(encoded)).unpack()
    const request = (host.payload as H.RequestT).value as H.UISaveRequestT
    const base = JSON.parse(String(request.baseJson)).files
    const ours = structuredClone(base)
    for (const c of request.save!.changes) {
      if (c.valueType === R.DocumentChangeValue.SetProject) ours['tsugumi.json'] = JSON.parse(String((c.value as R.SetProjectT).json))
      else if (c.valueType === R.DocumentChangeValue.SetLayout) ours['layout.json'] = JSON.parse(String((c.value as R.SetLayoutT).json))
      else assert.fail('Unexpected change')
    }
    const merged = hostDocumentReply({ baseJson: JSON.stringify(base), oursJson: JSON.stringify(ours), theirsJson: JSON.stringify(files) })
    assert.deepEqual(merged.conflicts, [])
    files = JSON.parse(merged.mergedJson)
    const result = new H.UISaveResultT()
    result.reply = new R.ReplyT(R.ReplyPayload.SaveResult, Object.assign(new R.SaveResultT(), { rev: ++revision }))
    result.snapshot = Object.assign(new R.ProjectSnapshotT(), { projectId: 'undo', rev: revision, projectJson: JSON.stringify(files['tsugumi.json']), layoutJson: JSON.stringify(files['layout.json']), run: new R.RunStatusT(), definitions: [Object.assign(new R.DefinitionT(), { id: 'a', cardJson: '{"template":"draft"}' })] })
    const builder = new fb.Builder()
    H.Message.finishMessageBuffer(builder, new H.MessageT(1n, H.Payload.Reply, new H.ReplyT(H.ReplyPayload.UISaveResult, result)).pack(builder))
    return { ok: true, json: async () => decodeReply(builder.asUint8Array(), true).value }
  }) as typeof fetch
  const edit = new RevisionEdit(); edit.reload(session.rev)
  notice({ cards: { a: { x: 0, y: 0 } }, groups: [{ id: 'row', scope: '', name: 'other', color: 'blue', cards: ['a'] }] })
  assert.equal(await edit.apply(session, false, x => { x.project = parse('{"name":"mine","canvas":{"cards":[{"id":"a"}],"connections":[]}}') }), true)
  assert.equal(session.groups([]).length, 1)
  notice({ cards: { a: { x: 0, y: 0 } }, groups: [] })
  await session.undo()
  assert.equal(str(get(session.project, 'name')), 'base')
  assert.deepEqual(session.groups([]), [])
  await (session as unknown as { saveOnce(): Promise<void> }).saveOnce()
  assert.equal(session.saveError, undefined)
  assert.deepEqual((files['layout.json'] as any).groups, [])
  await session.redo()
  assert.equal(str(get(session.project, 'name')), 'mine')
  assert.deepEqual(session.groups([]), [])
  await (session as unknown as { saveOnce(): Promise<void> }).saveOnce()
} finally { globalThis.fetch = realFetch }
console.log('undo of a merged rename preserves another writer’s group deletion: passed')
