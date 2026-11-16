import { cardIds } from './cardIds'
// Presentation integration: measurements go to RPC, a proposal is one layout
// edit, and a delayed answer cannot overwrite another edit or undo.
import assert from 'node:assert/strict'
import { Builder, ByteBuffer } from 'flatbuffers'
import fs from 'node:fs'
import path from 'node:path'
import { D, R } from '../src/shared/rpc'
import { parse, serialize, get, dbl, clone, JObj } from '../src/shared/json'
import type { LayoutProposal, LayoutScene } from '../src/model/layoutScene'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')
const { CanvasController } = await import('../src/model/canvas')
const { WorkflowTab } = await import('../src/model/workspace')

// The host service wire encodes the measured scene, preserving golden inputs.
const { encodeRequest, decodeReply } = await import('../server/serviceWire')
const { H } = await import('../src/shared/hostLayout')
const vectors: { name: string; request: string; scene: LayoutScene; around?: string[]; expect: LayoutProposal }[] = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'tests/fixtures/layout-golden.json'), 'utf8'))
// Rich specimens include both boundary bars, frame/note positions and port order.
for (const name of ['arrange: inner-scope topToBottom grid20',
  'untangle: inner-scope topToBottom grid20 arranged, around the first Card']) {
  const v = vectors.find(v => v.name === name)!
  const method = v.request === 'arrange' ? 'LayoutArrangeRequest' : 'LayoutUntangleRequest'
  const bytes = encodeRequest(method, { scene: v.scene, around: v.around ?? [] }, 1n, true)
  const message = H.Message.getRootAsMessage(new ByteBuffer(bytes)).unpack()
  const request = message.payload as H.RequestT
  assert.equal(request.valueType, H.RequestPayload[method])
  const value = request.value as H.LayoutArrangeRequestT | H.LayoutUntangleRequestT
  assert.equal(value.scene!.flow, v.scene.flow)
  const scene = value.scene!
  assert.equal(scene.grid, v.scene.grid)
  assert.equal(scene.snap, v.scene.snap)
  for (const k of ['cards', 'wires', 'frames', 'notes'] as const) assert.deepEqual(JSON.parse(JSON.stringify(scene[k])), v.scene[k])
  for (const side of ['inputs', 'outputs'] as const) {
    const bar = v.scene[side]
    if (!bar) { assert.equal(scene[side], null); continue }
    assert.deepEqual(JSON.parse(JSON.stringify(scene[side]!.ends)), bar.ends)
    assert.equal(scene[side]!.x, bar.at?.x ?? null)
    assert.equal(scene[side]!.y, bar.at?.y ?? null)
  }
  if (v.request === 'untangle') assert.deepEqual((value as H.LayoutUntangleRequestT).around, v.around ?? [])
  const place = (p: { id: string; x: number; y: number }) => new H.LayoutPlaceT(p.id, p.x, p.y)
  const proposal = new H.LayoutProposalT(v.expect.cards.map(place), v.expect.frames.map(place), v.expect.notes.map(place),
    v.expect.portOrders.map(o => new H.LayoutPortOrderT(o.id, o.inputs, o.outputs)))
  const reply = new H.MessageT(1n, H.Payload.Reply, new H.ReplyT(H.ReplyPayload.LayoutProposal, proposal))
  const builder = new Builder(1024); builder.finish(reply.pack(builder), 'TSH1')
  assert.deepEqual(JSON.parse(JSON.stringify(decodeReply(builder.asUint8Array(), true).value)), v.expect)
}

// Preparing a UI save carries the core's typed transaction unchanged over HTTP
// and Host RPC, including binary source bytes and revisions above JS precision.
{
  const save = Object.assign(new R.ProjectSaveRequestT(), { baseRev: 1n << 60n, opId: 'prepare', origin: 'ui',
    changes: [new R.DocumentChangeT(R.DocumentChangeValue.PutSource, new R.PutSourceT('definitions/a/data.bin', [0, 255, 128], 'hash'))] })
  const packed = new Builder(); packed.finish(save.pack(packed))
  const bytes = encodeRequest('LayoutPrepareSaveRequest', { project: 'p', saveBytes: Array.from(packed.asUint8Array()) }, 7n, true)
  const request = H.Message.getRootAsMessage(new ByteBuffer(bytes)).unpack().payload as H.RequestT
  const prepared = request.value as H.LayoutPrepareSaveRequestT
  assert.equal(prepared.save!.baseRev, save.baseRev)
  assert.deepEqual((prepared.save!.changes[0].value as R.PutSourceT).bytes, [0, 255, 128])
  const reply = new H.MessageT(7n, H.Payload.Reply, new H.ReplyT(H.ReplyPayload.LayoutPreparedSave, new H.LayoutPreparedSaveT(prepared.save)))
  const wire = new Builder(); H.Message.finishMessageBuffer(wire, reply.pack(wire))
  const dto = decodeReply(wire.asUint8Array(), true).value
  const restored = R.ProjectSaveRequest.getRootAsProjectSaveRequest(new ByteBuffer(new Uint8Array(dto.saveBytes))).unpack()
  assert.equal(restored.baseRev, save.baseRev)
  assert.deepEqual(restored.changes, save.changes)
}

// A UI consumes the host proposal without touching a document, revision or autosave.
// Real derivation agreement is covered by HostLayout.EveryGoldenRequestReturnsACorrelatedProposal.
// A person's immediate editor save materializes them in that same transaction.
{
  const sessions = [0].map(() => {
    const session = new ProjectSession('/unplaced', 'unplaced', 'unplaced')
    session.project = parse('{"canvas":{"cards":[{"id":"a"}],"connections":[]}}')
    session.confirmedProject = parse(serialize(session.project))
    session.confirmedDefinitions.set('a', parse('{"template":"draft"}'))
    session.projectId = 'p'; session.connected = true
    session.layout = parse('{}'); session.confirmedLayout = parse('{}')
    return session
  })
  const placed = '{"cards":{"a":{"x":120,"y":240}}}'
  let saves = 0, scheduled = 0
  for (const session of sessions) {
    session.scheduleSave = () => { scheduled++ }
    session.layoutHost = { ...session.layoutHost, complete: async () => placed,
      prepareSave: async (_project, baseRev, changes) => {
        assert.equal(baseRev, 0n)
        return [...changes, new R.DocumentChangeT(R.DocumentChangeValue.SetLayout, new R.SetLayoutT(placed))]
      } }
    session.client = { save: async (baseRev, opId, origin, changes) => {
      saves++
      const delta = Object.assign(new R.DocumentDeltaT(), { baseRev, rev: baseRev + 1n, opId, origin })
      for (const change of changes) {
        if (change.valueType === R.DocumentChangeValue.SetLayout) delta.layoutJson = (change.value as R.SetLayoutT).json
        if (change.valueType === R.DocumentChangeValue.PutDefinition) delta.definitionsUpsert.push(Object.assign(new R.DefinitionT(), change.value))
      }
      return Object.assign(new R.SaveResultT(), { rev: delta.rev, delta })
    } } as never
  }
  await Promise.all(sessions.map(session => session.refreshDerivedLayout()))
  for (const session of sessions) {
    assert.deepEqual(session.position('a'), { x: 120, y: 240 })
    assert.equal(session.rev, 0n)
    assert.equal(session.dirty, false)
    assert.equal(serialize(session.confirmedLayout), '{}\n')
  }
  assert.equal(saves, 0); assert.equal(scheduled, 0)
  assert.equal(await sessions[0].applyEdit(0n, session => session.updateMeta('a', meta => meta.set('description', 'Immediate edit'))), 1n)
  assert.equal(saves, 1)
  assert.equal(sessions[0].dirty, false)
  assert.deepEqual(sessions[0].position('a'), { x: 120, y: 240 })
}

const make = async () => {
  const session = cardIds(new ProjectSession('/p', 'k', 'p'))
  session.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  session.layout = parse('{"flow":"topToBottom"}')
  session.scheduleSave = () => {}
  session.client = {} as never
  await session.addCard('python', [], { x: 200, y: 0 }, 'source')
  await session.addCard('python', [], { x: 0, y: 300 }, 'left')
  await session.addCard('draft', [], { x: 500, y: 300 }, 'right')
  const ports = (names: string[]) => names.map(name => ({ name, kind: D.PortKind.value, optional: false }))
  session.resolution.set('source', { path: ['source'], inputs: [], outputs: ports(['a', 'b']), diagnostics: [] })
  session.resolution.set('left', { path: ['left'], inputs: ports(['in']), outputs: [], diagnostics: [] })
  await session.connect(['source', 'a'], ['right', ''], [])
  await session.connect(['source', 'b'], ['left', 'in'], [])
  return session
}
const orders = [{ id: 'source', inputs: [], outputs: ['b', 'a'] }]
const onlyOrders: LayoutProposal = { cards: [], frames: [], notes: [], portOrders: orders }
const before = (s: InstanceType<typeof ProjectSession>) => ({ layout: serialize(s.layout), project: serialize(s.project), steps: s.undoStack.length })

// Arrange applies positions, frame sizes, notes and orders together, including
// a frame still displayed beside its Card and a visual-only group.
{
  const s = await make(), note = await s.addNote([], { x: 3, y: 4 })
  s.displays.set('source', { runId: 'r', invocation: 1n, revision: 1n, value: { kind: 'text', text: 'shown' } })
  s.updateLayout([], l => l.set('groups', parse('[{"id":"g","title":"Group","cards":["source","left"]}]')))
  const was = before(s)
  let measured: LayoutScene | undefined
  s.layoutHost = { layoutArrange: async (scene: LayoutScene) => {
    measured = scene
    return { cards: [{ id: 'source', x: 20, y: 40 }, { id: 'left', x: 0, y: 200 }, { id: 'right', x: 300, y: 200 }],
      frames: [{ id: 'source', x: 500, y: 40 }], notes: [{ id: note, x: 600, y: 400 }], portOrders: orders }
  } } as never
  await s.arrange([])
  assert.equal(s.undoStack.length, was.steps + 1)
  assert.equal(serialize(s.project), was.project)
  assert.equal(measured!.cards[0].reorderable, true)
  assert.equal(measured!.wires[0].toPort, '', 'layout keeps Draft wire ends')
  assert.deepEqual(measured!.cards.find(c => c.id === 'right')!.inputs.map(p => p.name), [''], 'an unnamed geometric anchor locates the wire')
  assert.equal(measured!.frames[0].width, 320)
  assert.deepEqual(s.position('source'), { x: 20, y: 40 })
  assert.equal(dbl(get(s.displayFrame('source'), 'width')), 320)
  assert.equal(dbl(get(s.displayFrame('source'), 'height')), 170)
  assert.equal(dbl(get(s.note(note), 'x')), 600)
  assert.deepEqual(s.geometry('source').outputs, ['b', 'a'])
  assert.ok(get(s.layout, 'groups'))
  await s.undo()
  assert.equal(serialize(s.layout), was.layout)
  await s.redo()
  assert.deepEqual(s.geometry('source').outputs, ['b', 'a'])
}

// Untangle does not move Cards or rewrite named connections.
{
  const s = await make(), was = before(s), at = s.position('source')
  s.layoutHost = { layoutUntangle: async () => onlyOrders } as never
  await s.untangle([])
  assert.deepEqual(s.position('source'), at)
  assert.equal(serialize(s.project), was.project)
  assert.equal(s.undoStack.length, was.steps + 1)
  await s.undo()
  assert.equal(serialize(s.layout), was.layout)
}

// Drag positions and the delayed port order form one undo/redo step.
{
  const s = await make(), cv = new CanvasController(s, new WorkflowTab([])), was = before(s)
  s.layoutHost = { layoutUntangle: async (_scene: LayoutScene, around: string[]) => {
    assert.deepEqual(around, ['left'])
    return onlyOrders
  } } as never
  await cv.moveSelection(new Set(['left']), new Set(), new Set(), 1000, 0)
  assert.equal(s.undoStack.length, was.steps + 1)
  assert.deepEqual(s.geometry('source').outputs, ['b', 'a'])
  const after = serialize(s.layout)
  await s.undo()
  assert.equal(serialize(s.layout), was.layout)
  await s.redo()
  assert.equal(serialize(s.layout), after)
}

// A nested scope carries its boundary endpoints and hand-placed bars; Arrange
// returns the bars to following Cards and does not edit the outer layout.
{
  const s = await make()
  const outer = (await s.makeSubgraph(new Set(['source']), []))!
  s.resolution.set(outer + '/source', { ...s.resolution.get('source')!, path: [outer, 'source'] })
  s.setBoundaryPosition([outer], 'outputs', { x: 333, y: 444 })
  const beforeOuter = s.position(outer), was = before(s)
  let seen = false
  s.layoutHost = { layoutArrange: async (scene: LayoutScene) => {
    seen = true
    assert.deepEqual(scene.cards.map(c => c.id), ['source'])
    assert.deepEqual(scene.outputs?.at, { x: 333, y: 444 })
    assert.equal(scene.outputs?.ends.length, 2)
    assert.ok(scene.outputs!.ends.every(e => e.card === 'source' && ['a', 'b'].includes(e.port)))
    return { cards: [{ id: 'source', x: 40, y: 60 }], frames: [], notes: [], portOrders: orders }
  } } as never
  await s.arrange([outer])
  assert.ok(seen)
  assert.equal(s.boundaryPosition([outer], 'outputs'), undefined)
  assert.deepEqual(s.position(outer), beforeOuter)
  assert.deepEqual(s.position(outer + '/source'), { x: 40, y: 60 })
  assert.equal(serialize(s.project), was.project)
  await s.undo()
  assert.equal(serialize(s.layout), was.layout)
  assert.deepEqual(s.boundaryPosition([outer], 'outputs'), { x: 333, y: 444 })
}

// No answer for an obsolete scene is applied, including after an undo,
// reconnect, preferences/measurement change or a second edit during a drag.
for (const action of ['edit', 'undo', 'disconnect', 'measurement'] as const) {
  const s = await make()
  let resolve!: (p: LayoutProposal) => void
  s.layoutHost = { layoutUntangle: () => new Promise<LayoutProposal>(r => { resolve = r }) } as never
  const pending = s.untangle([])
  if (action === 'edit') s.edit(x => x.setPosition('left', { x: 999, y: 888 }))
  if (action === 'undo') await s.undo()
  if (action === 'disconnect') s.client = undefined
  if (action === 'measurement') s.displays.set('source', { runId: 'r', invocation: 1n, revision: 1n, value: { kind: 'text', text: 'new frame' } })
  const was = before(s)
  resolve(onlyOrders)
  await pending
  assert.deepEqual(before(s), was, `${action}: obsolete proposal is discarded`)
}
{
  const s = await make(), cv = new CanvasController(s, new WorkflowTab([]))
  let resolve!: (p: LayoutProposal) => void
  s.layoutHost = { layoutUntangle: () => new Promise<LayoutProposal>(r => { resolve = r }) } as never
  const pending = cv.moveSelection(new Set(['left']), new Set(), new Set(), 1000, 0)
  await s.undo()
  const was = before(s)
  resolve(onlyOrders)
  await pending
  assert.deepEqual(before(s), was, 'undo during a drag request cannot revive the drag or add another undo step')
}
{
  const s = await make(), was = before(s)
  s.layoutHost = { layoutArrange: async () => { throw new Error('layout failed') } } as never
  await s.arrange([])
  assert.deepEqual(before(s), was)
  assert.equal(s.notice, 'layout failed')
}
console.log(`layout RPC: two representative wire round trips; proposal application, undo and stale responses passed`)

// Save replies complete the same undo edit in both canvas and editor paths.
// Placement geometry itself is tested through the real host handler; this
// boundary supplies a completion so these tests isolate session/history state.
for (const editor of [false, true]) for (const lostReply of [false, true]) {
  const s = cardIds(new ProjectSession('/p', 'placement-undo', 'p'))
  s.project = parse('{"name":"t","canvas":{"cards":[{"id":"a"}],"connections":[]}}')
  s.confirmedProject = clone(s.project)
  s.confirmedDefinitions.set('a', parse('{"template":"draft"}'))
  s.layout = parse('{}'); s.confirmedLayout = parse('{}')
  s.projectId = 'p'; s.scheduleSave = () => {}
  s.refreshExternals = async () => {}
  s.layoutHost = { ...s.layoutHost, prepareSave: async (_project, _rev, changes) => {
    const layoutEdit = changes.find(c => c.valueType === R.DocumentChangeValue.SetLayout)
    const layout = parse(layoutEdit ? String((layoutEdit.value as R.SetLayoutT).json) : serialize(s.confirmedLayout)) as JObj
    const positions = (get(layout, 'cards') ?? parse('{}')) as JObj
    if (!positions.has('a')) positions.set('a', parse('{"x":0,"y":0}'))
    layout.set('cards', positions)
    return [...changes.filter(c => c !== layoutEdit).map(c => {
      if (c.valueType !== R.DocumentChangeValue.PutSource || !String((c.value as R.PutSourceT).path).endsWith('/layout.json')) return c
      return new R.DocumentChangeT(R.DocumentChangeValue.PutSource, new R.PutSourceT((c.value as R.PutSourceT).path,
        Array.from(new TextEncoder().encode('{"cards":{"inner":{"x":120,"y":240}}}')), null))
    }), new R.DocumentChangeT(R.DocumentChangeValue.SetLayout, new R.SetLayoutT(serialize(layout)))]
  } }
  const files = new Map<string, Uint8Array>()
  let receipt: R.SaveResultT
  s.client = { operation: async () => Object.assign(new R.OperationResultT(), { state: D.OperationState.completed,
    receipt: Object.assign(new R.OperationReceiptT(), { resultType: R.ReceiptValue.SaveResult, result: receipt }) }), save: async (baseRev, opId, origin, changes) => {
    const delta = Object.assign(new R.DocumentDeltaT(), { baseRev, rev: baseRev + 1n, opId, origin })
    for (const c of changes) {
      if (c.valueType === R.DocumentChangeValue.SetProject) delta.projectJson = (c.value as R.SetProjectT).json
      if (c.valueType === R.DocumentChangeValue.SetLayout) delta.layoutJson = (c.value as R.SetLayoutT).json
      if (c.valueType === R.DocumentChangeValue.PutDefinition) delta.definitionsUpsert.push(Object.assign(new R.DefinitionT(), c.value))
      if (c.valueType === R.DocumentChangeValue.RemoveDefinition) delta.definitionsRemoved.push((c.value as R.RemoveDefinitionT).id)
      if (c.valueType === R.DocumentChangeValue.PutSource) {
        const v = c.value as R.PutSourceT, file = String(v.path), bytes = new Uint8Array(v.bytes)
        files.set(file, bytes)
        delta.sourcesUpsert.push(Object.assign(new R.SourceInfoT(), { path: file, hash: 'saved-' + file, sizeBytes: BigInt(bytes.length) }))
      }
      if (c.valueType === R.DocumentChangeValue.RemoveSource) {
        const file = String((c.value as R.RemoveSourceT).path)
        files.delete(file); delta.sourcesRemoved.push(file)
      }
    }
    receipt = Object.assign(new R.SaveResultT(), { rev: delta.rev, delta })
    if (lostReply) throw new Error('save reply lost')
    return receipt
  } } as never
  const change = (x: InstanceType<typeof ProjectSession>) => {
    x.externalEdits.set('externals/imported/', { project: parse('{"canvas":{"cards":[{"id":"inner"}],"connections":[]}}'),
      layout: parse('{}'), definitions: new Map([['inner', { card: parse('{"template":"draft"}') }]]) })
  }
  if (editor) await s.applyEdit(0n, change)
  else { s.edit(change); await (s as unknown as { saveOnce(): Promise<void> }).saveOnce() }
  assert.equal(s.saveError, undefined)
  const saved = s.docState()
  assert.ok(get(saved.externals.get('externals/imported/')?.layout, 'cards', 'inner'))
  assert.ok(get(saved.layout, 'cards', 'a'))
  assert.equal(s.dirty, false)
  await s.undo()
  assert.equal(s.mergeConflict, undefined, 'the completed external is removable in one undo')
  assert.equal(s.saveError, undefined)
  assert.equal(s.effectiveExternals.has('externals/imported/'), false)
  assert.deepEqual(s.layout, saved.layout, 'undo retains the pre-existing Card place')
  await (s as unknown as { saveOnce(): Promise<void> }).saveOnce()
  assert.equal(files.has('externals/imported/layout.json'), false)
  await s.redo()
  assert.deepEqual(s.docState(), saved, 'redo restores the exact completed state')
}
