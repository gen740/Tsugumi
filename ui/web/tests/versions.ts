// A version's minimap fits its Cards and draws each with its canvas shape.
import assert from 'node:assert/strict'
import { VersionCommentT } from '../shared/gen/tsugumi/rpc/version-comment'

const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { miniMapLayout } = await import('../src/ui/versions')
const { cardKindNamed } = await import('../src/model/session')
const { CardGeometry } = await import('../src/model/canvasKit')
type CardKind = import('../src/model/session').CardKind
type CardGeometry = import('../src/model/canvasKit').CardGeometry
type CardNodeData = import('../src/model/canvasKit').CardNodeData

/** A Card with the ports a minimap gives it. */
function nodeData(kind: CardKind, fan: number): CardNodeData {
  const ports = (n: number) => Array.from({ length: n }, (_, i) => ({ name: `p${i}` }))
  return {
    title: 'c', kind, inputs: ports(kind === 'merge' ? fan : 1), outputs: ports(1), params: [],
    invocation: 0n, runContinues: false, errors: [], warnings: [], collapsed: false,
  }
}

const card = (id: string, x: number, y: number, kind: CardKind = 'code', fan = 0, collapsed = false) => ({ id, x, y, kind, fan, collapsed })
const overlap = (a: { x: number; y: number; w: number; h: number }, b: typeof a) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h

// Every Card stays inside the box, in the order it has on the canvas.
const wide = miniMapLayout([card('a', 100, 0), card('b', 5000, 300), card('c', 2500, -400)], 'leftToRight', 284, 132)
for (const c of wide.cards) {
  assert.ok(c.x >= 0 && c.x + c.w <= 284, `${c.id} is inside across`)
  assert.ok(c.y >= 0 && c.y + c.h <= wide.height && wide.height <= 132, `${c.id} is inside down`)
}
assert.ok(wide.cards[0].x < wide.cards[2].x && wide.cards[2].x < wide.cards[1].x)
assert.equal(wide.named, false, 'too small for names')
// A small graph is not blown up, and is centered with its names.
const small = miniMapLayout([card('a', 0, 0)], 'leftToRight', 284, 132)
assert.equal(small.cards[0].w, 220 * 0.3)
assert.equal(small.cards[0].x, (284 - small.cards[0].w) / 2)
assert.equal(small.named, true)
assert.equal(small.height, small.cards[0].h + 16, 'the box is only as tall as the graph')
assert.deepEqual(miniMapLayout([], 'leftToRight', 284, 132).cards, [])

// What is not a Card has the outline and size it has on the canvas, so a graph
// laid out on the canvas does not overlap here: start and two Cards feeding a Merge.
const flow = 'leftToRight'
const mergeGeometry = new CardGeometry(nodeData('merge', 2), flow), start = new CardGeometry(nodeData('trigger', 0), flow)
const placed = miniMapLayout([
  card('start', 0, 30, 'trigger'), card('merge', start.size.width + 10, 20, 'merge', 2),
  card('load', start.size.width + mergeGeometry.size.width + 40, -30), card('draft', start.size.width + mergeGeometry.size.width + 40, 60, 'draft'),
], flow, 284, 132)
const at = Object.fromEntries(placed.cards.map((c) => [c.id, c]))
assert.equal(at.start.shape, 'start')
assert.equal(at.merge.shape, 'merge')
assert.equal(at.load.shape, 'card')
assert.equal(at.draft.shape, 'card')
assert.equal(at.draft.draft, true, 'a Draft has its dashed outline')
assert.equal(at.load.draft, false)
for (const [i, a] of placed.cards.entries()) for (const b of placed.cards.slice(i + 1)) assert.ok(!overlap(a, b), `${a.id} and ${b.id} do not overlap`)
// The shapes are those of the canvas, scaled and at their place.
const k = at.merge.w / mergeGeometry.size.width
assert.ok(k > 0 && k <= 0.3)
assert.equal(at.merge.h, mergeGeometry.size.height * k)
assert.equal(at.start.w, start.size.width * k)
const outlineOf = (g: CardGeometry, c: { x: number; y: number }, scale: number) => {
  const o = g.outlineRect
  return g.outline({ x: c.x + o.x * scale, y: c.y + o.y * scale, width: o.width * scale, height: o.height * scale }, scale)
}
assert.equal(at.merge.outline, outlineOf(mergeGeometry, at.merge, k))
assert.equal(at.start.outline, outlineOf(start, at.start, k))
// The outline leaves the room the canvas leaves for its wires.
const leftmost = (d: string) => Math.min(...[...d.matchAll(/(-?[\d.]+),(-?[\d.]+)/g)].map((m) => Number(m[1])))
assert.ok(Math.abs(leftmost(at.merge.outline) - (at.merge.x + mergeGeometry.outlineRect.x * k)) < 1e-6, 'the outline leaves the room the canvas leaves for its wires')

// A branch is a diamond whose corners shrink with it; a merge is a triangle that
// grows with its inputs.
const diamond = miniMapLayout([card('b', 0, 0, 'branch')], flow, 284, 132).cards[0]
const canvasDiamond = new CardGeometry(nodeData('branch', 0), flow)
assert.equal(diamond.shape, 'branch')
assert.equal(diamond.w, canvasDiamond.size.width * 0.3)
assert.equal(diamond.outline, outlineOf(canvasDiamond, diamond, 0.3))
assert.notEqual(diamond.outline, canvasDiamond.outline({ x: diamond.x + canvasDiamond.outlineRect.x * 0.3, y: diamond.y, width: canvasDiamond.outlineRect.width * 0.3, height: canvasDiamond.outlineRect.height * 0.3 }))
const merge = (n: number, f: 'leftToRight' | 'topToBottom') => miniMapLayout([card('m', 0, 0, 'merge', n)], f, 284, 132).cards[0]
assert.equal(merge(2, flow).shape, 'merge')
assert.ok(merge(6, flow).h > merge(2, flow).h, 'more inputs, a wider edge')
assert.ok(merge(6, 'topToBottom').w > merge(2, 'topToBottom').w)
assert.notEqual(merge(2, flow).outline, merge(2, 'topToBottom').outline)

// A loop is a merge and a branch with a wire back: wires are the client's, so
// the two only need not overlap and keep their shapes.
const loop = miniMapLayout([card('m', 0, 0, 'merge', 2), card('step', 140, 0), card('b', 400, 0, 'branch')], flow, 284, 132)
assert.deepEqual(loop.cards.map((c) => c.shape), ['merge', 'card', 'branch'])
for (const [i, a] of loop.cards.entries()) for (const b of loop.cards.slice(i + 1)) assert.ok(!overlap(a, b))

// Only a Card or the start has a name inside it; a folded Card is its header.
const folded = miniMapLayout([card('a', 0, 0, 'code', 0, true)], flow, 284, 132).cards[0]
assert.equal(folded.h, CardGeometry.header * 0.3)
// The kinds as the daemon names them; an unreadable definition is a definition error.
assert.equal(cardKindNamed('merge'), 'merge')
assert.equal(cardKindNamed(''), 'invalid')
assert.equal(cardKindNamed('something new'), 'invalid')

// The UI asks only for the change record. No client-side diff, model or note write.
const { ProjectSession } = await import('../src/model/session')
const { layoutHost } = await import('../src/wire/hostLayout')
const rawCanvas = { layoutJson: '{"cards":{"plan":{"x":120,"y":80}}}', cards: [{ id: 'plan', kind: 'draft', fan: 0 }], wires: [] }
const hostCanvas = { cards: [card('plan', 120, 80, 'draft')], wires: [], flow: 'topToBottom' }
const fetchBefore = globalThis.fetch
let failNote = false
const calls: { path: string; body?: Record<string, unknown> }[] = []
globalThis.fetch = async (url, init) => {
  const path = String(url)
  const body = init?.body ? JSON.parse(String(init.body)) : undefined
  calls.push({ path, body })
  assert.equal(path, '/api/versions/note')
  return new Response(JSON.stringify(failNote ? { error: 'retry me' } : { text: 'Host-owned note' }), { status: failNote ? 503 : 200 })
}
try {
  // Avoid connecting a project: only the presentation methods under test run.
  const session = Object.create(ProjectSession.prototype) as InstanceType<typeof ProjectSession>
  Object.assign(session, { client: { versionCanvas: async (commit: string) => { assert.equal(commit, 'version'); return rawCanvas } }, layoutHost: { ...layoutHost, versionCanvas: async (input) => {
    assert.deepEqual(input, rawCanvas)
    return hostCanvas
  } }, versionCanvases: new Map(), dir: '/project', projectId: 'project', projectHost: {
    async comments(project: string, commit: string) {
      assert.equal(project, 'project')
      assert.equal(commit, 'version')
      return [new VersionCommentT('plan', 'Ask', 'resolved', null, 'agent', ['plan'])]
    },
  }, versionComments: new Map(), versionNoteRequests: new Map(), versionNoteBusy: new Set(), versionNoteErrors: new Map(), versionNotes: new Map(), changed() {} })
  await session.loadVersionCanvas('version')
  assert.deepEqual(session.versionCanvases.get('version'), hostCanvas)
  await session.loadVersionNote('version')
  assert.equal(session.versionNotes.get('version'), 'Host-owned note')
  assert.equal(session.versionComments.get('version')?.[0].card, 'plan')
  assert.deepEqual(calls.at(-1), { path: '/api/versions/note', body: { path: '/project', commit: 'version' } })
  failNote = true
  await session.loadVersionNote('version')
  assert.equal(session.versionNoteBusy.size, 0)
  assert.equal(session.versionNoteErrors.get('version'), 'retry me')
  failNote = false
  await session.loadVersionNote('version')
  assert.equal(session.versionNoteErrors.size, 0)
  assert.equal(session.versionNotes.get('version'), 'Host-owned note')
  const immediateFetch = globalThis.fetch
  const pending: { release?: (response: Response) => void } = {}
  globalThis.fetch = (url, init) => String(url) === '/api/versions/note'
    ? new Promise<Response>((resolve) => { pending.release = resolve }) : immediateFetch(url, init)
  const oldRequest = session.loadVersionNote('version')
  while (!pending.release) await new Promise((resolve) => setTimeout(resolve, 0))
  globalThis.fetch = immediateFetch
  await session.loadVersionNote('version')
  pending.release(new Response(JSON.stringify({ text: 'Late response for an earlier request' })))
  await oldRequest
  assert.equal(session.versionNotes.get('version'), 'Host-owned note', 'a late response cannot overwrite a newer one')
} finally {
  globalThis.fetch = fetchBefore
}
console.log('version canvas and comments: host requests; notes: failure display, retry and late responses passed')
