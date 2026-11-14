import { cardIds } from './cardIds'
// Presentation integration: measurements go to RPC, a proposal is one layout
// edit, and a delayed answer cannot overwrite another edit or undo.
import assert from 'node:assert/strict'
import { Builder, ByteBuffer } from 'flatbuffers'
import fs from 'node:fs'
import path from 'node:path'
import { D } from '../src/shared/rpc'
import { parse, serialize, get, dbl } from '../src/shared/json'
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
const vectors: { request: string; scene: LayoutScene; around?: string[]; expect: LayoutProposal }[] = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'tests/fixtures/layout-golden.json'), 'utf8'))
for (const v of vectors) {
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
  session.updateCard('right', d => d.set('inputs', parse('[{"name":"in"}]')))
  await session.connect(['source', 'a'], ['right', 'in'], [])
  await session.connect(['source', 'b'], ['left', 'in'], [])
  return session
}
const orders = [{ id: 'source', inputs: [], outputs: ['b', 'a'] }]
const onlyOrders: LayoutProposal = { cards: [], frames: [], notes: [], portOrders: orders }
const before = (s: InstanceType<typeof ProjectSession>) => ({ layout: serialize(s.layout), project: serialize(s.project), steps: s.undoStack.length })

// Arrange applies positions, frame sizes, notes and orders together, including
// a frame still displayed beside its Card and a visual-only group.
{
  const s = await make(), note = s.addNote([], { x: 3, y: 4 })
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
  assert.equal(measured!.frames[0].width, 320)
  assert.deepEqual(s.position('source'), { x: 20, y: 40 })
  assert.equal(dbl(get(s.displayFrame('source'), 'width')), 320)
  assert.equal(dbl(get(s.displayFrame('source'), 'height')), 170)
  assert.equal(dbl(get(s.note(note), 'x')), 600)
  assert.deepEqual(s.geometry('source').outputs, ['b', 'a'])
  assert.ok(get(s.layout, 'groups'))
  s.undo()
  assert.equal(serialize(s.layout), was.layout)
  s.redo()
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
  s.undo()
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
  s.undo()
  assert.equal(serialize(s.layout), was.layout)
  s.redo()
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
  s.undo()
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
  if (action === 'undo') s.undo()
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
  s.undo()
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
console.log(`layout RPC: ${vectors.length} golden round trips; proposal application, undo and stale responses passed`)
