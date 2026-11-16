// Force the placement write at the HTTP save boundary, after the UI captured
// its base. The merge is the production C++ host implementation, not a JS copy.
import assert from 'node:assert/strict'
import * as fb from 'flatbuffers'
import * as H from '../src/gen/tsugumi/host'
import { R, D, RPCClient } from '../src/shared/rpc'
import { ProjectSession } from '../src/model/session'
import { CardEditorModel } from '../src/ui/cardSettings'
import { RevisionEdit } from '../src/ui/revisionEdit'
import { parse, clone, get, str } from '../src/shared/json'
import { cardIds } from './cardIds'
import { hostDocumentReply } from './hostDocument'
import { encodeRequest, decodeReply } from '../server/serviceWire'

const realFetch = globalThis.fetch
try {
  for (const entry of ['description', 'project', 'project noticed', 'layout', 'import', 'description conflict', 'layout conflict', 'group conflict']) {
    const session = cardIds(new ProjectSession('/placement', 'placement', 'placement'))
    session.projectId = 'placement'
    session.scheduleSave = () => {}
    session.project = parse('{"name":"base","canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}}')
    session.confirmedProject = clone(session.project)
    session.layout = parse('{"cards":{"a":{"x":0,"y":0}}}')
    if (entry === 'group conflict') session.layout = parse('{"cards":{"a":{"x":0,"y":0}},"groups":[{"id":"row","scope":"","name":"base","color":"blue","cards":["a"]}]}')
    session.confirmedLayout = clone(session.layout)
    session.confirmedDefinitions.set('a', parse('{"template":"draft"}'))
    session.confirmedDefinitions.set('b', parse('{"template":"draft"}'))
    session.client = new RPCClient({ call: async () => { throw new Error('The UI must save through the Host') }, close() {} })
    let writes = 0, savedFiles: Record<string, unknown> = {}
    globalThis.fetch = (async (url, options) => {
      if (url === '/api/import/project') return { ok: true, json: async () => ({ name: 'inner', files: {
        'tsugumi.json': btoa('{"name":"inner","canvas":{"cards":[],"connections":[]}}'), 'layout.json': btoa('{}'),
      } }) }
      assert.equal(url, '/api/document/save')
      const body = JSON.parse(String(options?.body))
      // Exercise the HTTP -> typed Host request adapter too, including uint64.
      const host = H.Message.getRootAsMessage(new fb.ByteBuffer(encodeRequest('UISaveRequest', body, 1n, true))).unpack()
      const request = (host.payload as H.RequestT).value as H.UISaveRequestT
      assert.equal(request.save!.baseRev, 0n)
      const base = JSON.parse(String(request.baseJson))
      assert.equal(base.files['layout.json'].cards.b, undefined, 'b had no place when the UI read')
      const ours = structuredClone(base.files), theirs = structuredClone(base.files)
      // This is the forced interleaving: placement commits now. Its change
      // notification is withheld so the response must catch the UI up itself.
      theirs['layout.json'].cards.b = { x: 400, y: 0 }
      if (entry === 'group conflict') theirs['layout.json'].groups = []
      if (entry === 'layout conflict') theirs['layout.json'].cards.a.x = 600
      if (entry === 'description conflict') theirs['definitions/a/_meta.json'] = { description: 'theirs' }
      for (const change of request.save!.changes) {
        if (change.valueType === R.DocumentChangeValue.SetProject) ours['tsugumi.json'] = JSON.parse(String((change.value as R.SetProjectT).json))
        if (change.valueType === R.DocumentChangeValue.SetLayout) ours['layout.json'] = JSON.parse(String((change.value as R.SetLayoutT).json))
        if (change.valueType === R.DocumentChangeValue.PutDefinition) {
          const d = change.value as R.PutDefinitionT, dir = `definitions/${d.id}/`
          ours[dir + (d.linkJson ? '_link.json' : '_card.json')] = JSON.parse(String(d.linkJson ?? d.cardJson))
          if (d.metaJson) ours[dir + '_meta.json'] = JSON.parse(String(d.metaJson))
        }
        if (change.valueType === R.DocumentChangeValue.PutSource) {
          const put = change.value as R.PutSourceT
          ours[String(put.path)] = JSON.parse(new TextDecoder().decode(Uint8Array.from(put.bytes)))
        }
      }
      const merge = hostDocumentReply({ baseJson: JSON.stringify(base.files), oursJson: JSON.stringify(ours), theirsJson: JSON.stringify(theirs) })
      const result = new H.UISaveResultT()
      if (merge.conflicts.length) {
        assert.match(merge.conflicts.join(', '), entry === 'description conflict' ? /_meta.json/ : /layout.json/)
        result.reply = new R.ReplyT(R.ReplyPayload.Failure, Object.assign(new D.FailureT(), {
          kind: D.FailureKind.revision_conflict, diagnostic: merge.conflicts.join(', '), currentRev: 1n,
        }))
        savedFiles = theirs
      } else {
        writes++
        savedFiles = JSON.parse(merge.mergedJson)
        result.reply = new R.ReplyT(R.ReplyPayload.SaveResult, Object.assign(new R.SaveResultT(), { rev: 2n }))
        result.snapshot = Object.assign(new R.ProjectSnapshotT(), {
          projectId: 'placement', rev: 2n, projectJson: JSON.stringify(savedFiles['tsugumi.json']), layoutJson: JSON.stringify(savedFiles['layout.json']),
          run: new R.RunStatusT(), definitions: Object.keys(savedFiles).filter(p => /^definitions\/[^/]+\/_(card|link)\.json$/.test(p)).map(p => p.split('/')[1]).map(id => Object.assign(new R.DefinitionT(), {
            id, cardJson: JSON.stringify(savedFiles[`definitions/${id}/_card.json`] ?? {}),
            linkJson: savedFiles[`definitions/${id}/_link.json`] ? JSON.stringify(savedFiles[`definitions/${id}/_link.json`]) : null,
            metaJson: savedFiles[`definitions/${id}/_meta.json`] ? JSON.stringify(savedFiles[`definitions/${id}/_meta.json`]) : null,
          })),
        })
      }
      const builder = new fb.Builder()
      H.Message.finishMessageBuffer(builder, new H.MessageT(1n, H.Payload.Reply, new H.ReplyT(H.ReplyPayload.UISaveResult, result)).pack(builder))
      return { ok: true, json: async () => decodeReply(builder.asUint8Array(), true).value }
    }) as typeof fetch
    const editor = new CardEditorModel(session, 'a')
    if (entry.startsWith('description')) {
      editor.meta = parse('{"description":"mine"}')
      assert.equal(await editor.apply(), entry === 'description', editor.revisionEdit.error ?? editor.message)
      if (entry.endsWith('conflict')) {
        assert.equal(str(get(editor.meta, 'description')), 'mine')
        assert.match(editor.revisionEdit.error ?? '', /_meta.json/)
        assert.equal(session.dirty, false)
      }
    } else if (entry === 'group conflict') {
      const edit = new RevisionEdit(); edit.reload(session.rev)
      const delta = Object.assign(new R.DocumentDeltaT(), { baseRev: 0n, rev: 1n,
        layoutJson: '{"cards":{"a":{"x":0,"y":0}},"groups":[]}' })
      ;(session as unknown as { applyDelta(d: R.DocumentDeltaT): void }).applyDelta(delta)
      assert.equal(await edit.apply(session, false, x => x.updateGroups([], groups => (groups[0] as import('../src/shared/json').JObj).set('name', 'mine'))), false)
      assert.match(edit.error ?? '', /groups/)
      assert.equal(session.dirty, false)
      assert.deepEqual(session.groups([]), [])
    } else if (entry.startsWith('project')) {
      const edit = new RevisionEdit(); edit.reload(session.rev)
      if (entry === 'project noticed') {
        const delta = Object.assign(new R.DocumentDeltaT(), { baseRev: 0n, rev: 1n,
          layoutJson: '{"cards":{"a":{"x":0,"y":0},"b":{"x":400,"y":0}}}' })
        ;(session as unknown as { applyDelta(d: R.DocumentDeltaT): void }).applyDelta(delta)
      }
      assert.equal(await edit.apply(session, false, x => { x.project = parse('{"name":"mine","canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}}') }), true)
    } else {
      if (entry.startsWith('layout')) session.edit(x => { x.setPosition('a', { x: 200, y: 0 }) })
      else await session.importProject('/inner', [], { x: 200, y: 400 })
      await (session as unknown as { saveOnce(): Promise<void> }).saveOnce()
      assert.equal(!!session.saveError, entry.endsWith('conflict'))
      if (entry.endsWith('conflict')) assert.match(session.saveError!, /layout.json/)
    }
    if (entry.startsWith('description')) assert.equal((savedFiles['definitions/a/_meta.json'] as { description: string }).description, entry === 'description' ? 'mine' : 'theirs')
    assert.equal(writes, entry.endsWith('conflict') ? 0 : 1)
    assert.deepEqual((savedFiles['layout.json'] as any).cards.b, { x: 400, y: 0 })
    if (entry === 'import') assert.ok(session.instance('inner'), 'the imported Card remains saved')
    if (!entry.endsWith('conflict')) assert.equal(session.position('b').x, 400, 'the reply brings in placement even without its notice')
  }
} finally { globalThis.fetch = realFetch }
console.log('placement between the UI read and every save entry: passed')
