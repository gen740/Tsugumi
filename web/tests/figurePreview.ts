// A chart preview on the canvas: which picture a frame holds while results
// change, and how it may be shown (the Card setting `displayMode`). A picture
// is never shown at full strength as a result it is not a picture of.
import assert from 'node:assert/strict'
import { FigurePicture, FigurePreview, FigureState, FigureWanted, ThumbnailRequest, figureLook, figureMode } from '../src/model/moduleThumbnails'
import { parse } from '../src/shared/json'
import * as R from '../src/gen/tsugumi/rpc'
import * as D from '../src/gen/tsugumi/data'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')
const { FrameRenderer } = await import('../src/ui/moduleRenderer')

const tick = () => new Promise(r => setTimeout(r, 0))
const want = (json: string, lineage = 'r1#1', size = 160): FigureWanted => ({ name: 'plotly', json, width: size, height: size, lineage })
const update = (running = false, stale = false): FigureState => ({ mode: 'update', running, stale })
const live = (running = false, stale = false): FigureState => ({ mode: 'live', running, stale })

/** A queue whose pictures are finished by hand, in the order they were asked for. */
function queue() {
  const asked: string[] = [], lingers: number[] = []
  const open = new Map<string, (image: string | undefined) => void>()
  let fail: string | undefined
  return {
    asked, lingers,
    request(r: ThumbnailRequest, signal: AbortSignal, lingerMs = 0) {
      asked.push(r.json); lingers.push(lingerMs)
      return new Promise<string | undefined>((resolve, reject) => {
        open.set(r.json, image => (fail === r.json ? reject(new Error('bad figure')) : resolve(image)))
        signal.addEventListener('abort', () => resolve(undefined), { once: true })
      })
    },
    async finish(json: string) { open.get(json)!(`png:${json}`); await tick() },
    async failing(json: string) { fail = json; open.get(json)!(undefined); await tick() },
  }
}
const preview = (q: ReturnType<typeof queue>) => {
  let changes = 0
  const p = new FigurePreview(q, () => { changes++ }, async (name, json, w, h) => `${name}:${json}:${w}:${h}`)
  return { p, changes: () => changes }
}

assert.equal(figureMode(undefined), 'update', 'update is the default')
assert.equal(figureMode('live'), 'live')
assert.equal(figureMode('smooth'), 'update')

// (a) The content changes while the old picture is shown.
{
  const old: FigurePicture = { ...want('old'), image: 'png:old' }
  // Update: the old picture stays, dimmed, until the new one is ready. Never at full strength.
  for (const running of [false, true]) {
    assert.deepEqual(figureLook(want('new'), old, update(running)), { image: 'png:old', dimmed: true, waiting: true })
  }
  assert.deepEqual(figureLook(want('new', 'r2#1'), old, update()), { image: 'png:old', dimmed: true, waiting: true }, 'another run: dimmed too')
  // Live: the previous revision of the invocation that is running stays at full strength (no gap between revisions)...
  assert.deepEqual(figureLook(want('new'), old, live(true)), { image: 'png:old', dimmed: false, waiting: false })
  // ...but not once the Card stopped, and never a picture of another run or invocation.
  assert.deepEqual(figureLook(want('new'), old, live(false)), { image: 'png:old', dimmed: true, waiting: true })
  assert.deepEqual(figureLook(want('new', 'r2#1'), old, live(true)), { image: 'png:old', dimmed: true, waiting: true })
  assert.deepEqual(figureLook(want('new', 'r1#2'), old, live(true)), { image: 'png:old', dimmed: true, waiting: true })
  // A same-length update is a different result as well.
  assert.equal(figureLook(want('{"y":[2]}'), { ...want('{"y":[1]}'), image: 'p' }, update()).dimmed, true)
  assert.equal(figureLook({ ...want('old'), name: 'vega-lite' }, old, update()).dimmed, true, 'another module')

  // The picture of the wanted content: full strength, except in Update while
  // the Card or one upstream runs (it is about to be replaced).
  assert.deepEqual(figureLook(want('old'), old, update()), { image: 'png:old', dimmed: false, waiting: false })
  assert.equal(figureLook(want('old'), old, update(true)).dimmed, true, 'the Card runs')
  assert.equal(figureLook(want('old'), old, update(false, true)).dimmed, true, 'an upstream Card runs')
  assert.equal(figureLook(want('old'), old, live(true, true)).dimmed, false, 'live follows the run')
  // The same content at another size is still this result's picture (zoom, resize).
  assert.deepEqual(figureLook(want('old', 'r1#1', 320), old, update()), { image: 'png:old', dimmed: false, waiting: false })
  // Nothing to show yet: the note, and a failure only for the content it is of.
  assert.deepEqual(figureLook(want('new'), undefined, update()), { dimmed: false, waiting: true })
  assert.deepEqual(figureLook(want('new'), { ...want('new'), error: 'bad' }, update()), { dimmed: false, waiting: false, error: 'bad' })
  assert.deepEqual(figureLook(want('new'), { ...want('old'), error: 'bad' }, update()), { dimmed: false, waiting: true })

  // The frame itself: the old picture is held until the new one arrives, then replaced.
  const q = queue(), { p } = preview(q)
  p.want(want('old')); await tick(); await q.finish('old')
  assert.equal(p.picture?.json, 'old')
  p.want(want('new')); await tick()
  assert.equal(p.picture?.json, 'old', 'held while the next is made')
  assert.equal(figureLook(want('new'), p.picture, update()).dimmed, true)
  await q.finish('new')
  assert.deepEqual(figureLook(want('new'), p.picture, update()), { image: 'png:new', dimmed: false, waiting: false })
  // A picture that arrives for content no longer wanted is not shown.
  p.want(want('a')); await tick(); p.want(want('b')); await tick()
  await q.finish('a')
  assert.equal(p.picture?.json, 'new', 'a late picture of superseded content is dropped')
  await q.finish('b')
  assert.equal(p.picture?.json, 'b')
}

// (c) A frame that left the view drops its picture and gets it back on return.
{
  const q = queue(), { p, changes } = preview(q)
  p.want(want('fig')); await tick(); await q.finish('fig')
  p.want(undefined)
  assert.equal(p.picture, undefined, 'out of view: nothing is kept')
  assert.equal(changes(), 2)
  p.want(want('fig')); await tick()
  assert.deepEqual(q.asked, ['fig', 'fig'], 'asked again (the queue answers from its cache)')
  await q.finish('fig')
  assert.deepEqual(figureLook(want('fig'), p.picture, update()), { image: 'png:fig', dimmed: false, waiting: false })
  // Leaving while a picture is being made: it is not shown afterwards.
  p.want(want('next')); await tick(); p.want(undefined)
  await q.finish('next')
  assert.equal(p.picture, undefined)
  // A frame with no size yet asks for nothing.
  p.want(want('fig', 'r1#1', 0)); await tick()
  assert.equal(q.asked.length, 3)
}

// (e) Rapid revisions of one invocation.
{
  // Live: the one being drawn is finished, the newest waits, those between are skipped.
  const q = queue(), { p } = preview(q)
  p.want(want('r1'), true); await tick()
  for (const r of ['r2', 'r3', 'r4']) { p.want(want(r), true); await tick() }
  assert.deepEqual(q.asked, ['r1'], 'one picture at a time for this frame')
  assert.ok(q.lingers[0] > 0, 'a followed figure keeps the renderer loaded between revisions')
  await q.finish('r1')
  assert.equal(p.picture?.json, 'r1')
  assert.deepEqual(figureLook(want('r4'), p.picture, live(true)), { image: 'png:r1', dimmed: false, waiting: false }, 'the stream advances without a gap')
  await tick()
  assert.deepEqual(q.asked, ['r1', 'r4'])
  p.want(want('r5'), true); await tick()
  await q.finish('r4'); await tick()
  assert.equal(p.picture?.json, 'r4')
  await q.finish('r5')
  // The run ended: the last revision is the result, at full strength.
  assert.deepEqual(figureLook(want('r5'), p.picture, live(false)), { image: 'png:r5', dimmed: false, waiting: false })
  // A revision that cannot be drawn while a newer one waits leaves the last picture.
  p.want(want('bad'), true); await tick(); p.want(want('r7'), true); await tick()
  await q.failing('bad'); await tick()
  assert.equal(p.picture?.json, 'r5')
  await q.finish('r7')
  assert.equal(p.picture?.json, 'r7')
  // Another invocation does not wait behind the last one's picture.
  p.want(want('s1'), true); await tick(); p.want(want('t1', 'r1#2'), true); await tick()
  assert.deepEqual(q.asked.slice(-2), ['s1', 't1'])
  await q.finish('s1')
  assert.equal(p.picture?.json, 'r7', 'the superseded invocation is not shown late')
  await q.finish('t1')
  assert.equal(p.picture?.json, 't1')
}
{
  // Update: each revision replaces the request; the last figure stays, dimmed
  // (the Card runs), and only the revision still wanted is shown.
  const q = queue(), { p } = preview(q)
  p.want(want('done')); await tick(); await q.finish('done')
  for (const r of ['r1', 'r2', 'r3']) {
    p.want(want(r)); await tick()
    assert.deepEqual(figureLook(want(r), p.picture, update(true)), { image: 'png:done', dimmed: true, waiting: true })
  }
  assert.ok(q.lingers.every(l => l === 0))
  await q.finish('r1'); await q.finish('r2')
  assert.equal(p.picture?.json, 'done')
  await q.finish('r3')
  assert.equal(figureLook(want('r3'), p.picture, update(true)).dimmed, true, 'still running: about to be replaced')
  assert.deepEqual(figureLook(want('r3'), p.picture, update(false)), { image: 'png:r3', dimmed: false, waiting: false })
}

// (d) The session's own displays: a new run, a selected earlier run, and the
// state of the Card and of those upstream of it.
{
  const s = new ProjectSession('/p', 'k', 'p')
  const project = '{"name":"t","canvas":{"cards":[{"id":"src"},{"id":"mid"},{"id":"plot"},{"id":"side"}],"connections":[' +
    '{"from":{"card":"src","port":"out"},"to":{"card":"mid","port":"in"}},{"from":{"card":"mid","port":"out"},"to":{"card":"plot","port":"in"}}]}}'
  s.project = parse(project)
  s.confirmedProject = parse(project)
  s.confirmedDefinitions.set('plot', parse('{"language":"python"}'))
  s.confirmedDefinitions.set('side', parse('{"language":"python","displayMode":"live"}'))
  const inner = s as unknown as { applyDisplay(d: D.DisplayT): void }
  const show = (card: string, runId: string, invocation: bigint, revision: bigint, json: string) => inner.applyDisplay(Object.assign(new D.DisplayT(), {
    card: Object.assign(new D.CardPathT(), { segments: [card] }), runId, invocation, revision,
    valueType: D.DisplayValue.DisplayModule, value: Object.assign(new D.DisplayModuleT(), { module: '/nix/store/00000000000000000000000000000000-tsugumi-view-plotly', configJson: json, blobs: [] }),
  }))
  const wanted = (card: string): FigureWanted => {
    const d = s.displays.get(card)!
    assert.equal(d.value.kind, 'module')
    const v = d.value as { name: string; json: string }
    return { name: v.name, json: v.json, width: 160, height: 160, lineage: `${d.runId}#${d.invocation}` }
  }
  const state = (card: string, st: D.CardState) => s.cardStates.set(card, { state: st, invocation: 1n, phase: D.Phase.execute, stages: [], cached: false } as never)
  const run = (st: D.RunState) => { s.runStatus = Object.assign(new R.RunStatusT(), { runId: 'r2', state: st }) }

  assert.equal(s.figureState('plot').mode, 'update')
  assert.equal(s.figureState('side').mode, 'live')

  const q = queue(), { p } = preview(q)
  const look = () => figureLook(wanted('plot'), p.picture, s.figureState('plot'))
  const follow = () => { const f = s.figureState('plot'); p.want(wanted('plot'), f.mode === 'live' && f.running) }
  show('plot', 'r1', 1n, 1n, '{"y":[1]}'); follow(); await tick(); await q.finish('{"y":[1]}')
  assert.deepEqual(look(), { image: 'png:{"y":[1]}', dimmed: false, waiting: false })

  // A new run starts upstream: the figure of the last run is about to be replaced.
  run(D.RunState.running); state('src', D.CardState.running)
  assert.deepEqual(s.figureState('plot'), { mode: 'update', running: false, stale: true })
  assert.equal(look().dimmed, true, 'dimmed while a Card upstream runs')
  assert.deepEqual(s.figureState('side'), { mode: 'live', running: false, stale: false }, 'a Card not downstream is untouched')
  state('src', D.CardState.succeeded); state('mid', D.CardState.succeeded); state('plot', D.CardState.running)
  assert.deepEqual(s.figureState('plot'), { mode: 'update', running: true, stale: false })
  assert.equal(look().dimmed, true, 'dimmed while the Card runs')
  // Its result arrives: the old figure is held dimmed, then replaced.
  show('plot', 'r2', 1n, 1n, '{"y":[2]}'); follow(); await tick()
  assert.deepEqual(look(), { image: 'png:{"y":[1]}', dimmed: true, waiting: true })
  await q.finish('{"y":[2]}')
  state('plot', D.CardState.succeeded); run(D.RunState.succeeded)
  assert.deepEqual(look(), { image: 'png:{"y":[2]}', dimmed: false, waiting: false })

  // An earlier run is selected in the history: its display replaces this one.
  show('plot', 'r1', 1n, 1n, '{"y":[1]}'); follow(); await tick()
  assert.deepEqual(look(), { image: 'png:{"y":[2]}', dimmed: true, waiting: true }, 'not the selected run\'s figure: dimmed')
  await q.finish('{"y":[1]}')
  assert.deepEqual(look(), { image: 'png:{"y":[1]}', dimmed: false, waiting: false })
  assert.equal(p.picture?.lineage, 'r1#1')

  // After the run nothing is dimmed, whatever the Cards' last states were.
  state('src', D.CardState.running)
  assert.deepEqual(s.figureState('plot'), { mode: 'update', running: false, stale: false })
}

// (b) The renderer's frame: a reply that arrives after its request timed out
// answers nothing, and neither does a frame that was closed.
{
  type Listener = (event: { source: unknown; data: unknown }) => void
  const listeners = new Set<Listener>()
  const win = page.window as Record<string, unknown>
  const before = { add: win.addEventListener, remove: win.removeEventListener, create: (page.document as Record<string, unknown>).createElement, body: (page.document as Record<string, unknown>).body }
  win.addEventListener = (_: string, l: Listener) => { listeners.add(l) }
  win.removeEventListener = (_: string, l: Listener) => { listeners.delete(l) }
  const frames: { contentWindow: { posted: { requestId: number; json: string }[]; postMessage(m: never): void }; removed: boolean }[] = []
  Object.assign(page.document as object, {
    createElement: () => {
      const frame = {
        style: {}, removed: false, setAttribute() {}, remove() { frame.removed = true },
        contentWindow: { posted: [] as { requestId: number; json: string }[], postMessage(m: never) { this.posted.push(m) } },
      }
      frames.push(frame)
      return frame
    },
    body: { append() {} },
  })
  const send = (frame: (typeof frames)[number], data: unknown) => { for (const l of [...listeners]) l({ source: frame.contentWindow, data }) }
  // A PNG header (signature, IHDR, 4 x 2 pixels) is all the renderer reads of the picture.
  const header = new Uint8Array(33)
  header.set([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52, 0, 0, 0, 4, 0, 0, 0, 2])
  const png = (tag: string) => `data:image/png;base64,${btoa(String.fromCharCode(...header))}${tag}`
  const req = (json: string): ThumbnailRequest => ({ key: json, name: 'plotly', json, width: 16, height: 16 })
  try {
    const renderer = new FrameRenderer(20, async () => 'export function initialise() {}')
    const first = renderer.draw(req('first'))
    send(frames[0], { tsugumi: 'ready' })
    await assert.rejects(first, /timed out/)
    const id1 = frames[0].contentWindow.posted[0].requestId
    let settled: unknown = 'pending'
    const second = renderer.draw(req('second')).then(v => (settled = v), e => (settled = e))
    await tick()
    const id2 = frames[0].contentWindow.posted[1].requestId
    assert.notEqual(id1, id2)
    send(frames[0], { tsugumi: 'state', state: 'ready', requestId: id1, image: png('late') })
    await tick()
    assert.equal(settled, 'pending', 'the late reply of the request that timed out answers nothing')
    send(frames[0], { tsugumi: 'state', state: 'ready', requestId: id2, image: png('second') })
    await second
    assert.deepEqual(settled, { image: png('second'), pixelBytes: 4 * 2 * 4 })

    // The queue closes a renderer that failed: the next request gets a new
    // frame, and whatever the old one still says is not this one's answer.
    renderer.close()
    assert.equal(frames[0].removed, true)
    settled = 'pending'
    const third = renderer.draw(req('third')).then(v => (settled = v), e => (settled = e))
    assert.equal(frames.length, 2)
    send(frames[0], { tsugumi: 'ready' })
    send(frames[1], { tsugumi: 'ready' })
    await tick()
    const id3 = frames[1].contentWindow.posted[0].requestId
    send(frames[0], { tsugumi: 'state', state: 'ready', requestId: id3, image: png('old-frame') })
    await tick()
    assert.equal(settled, 'pending', 'a closed frame cannot answer')
    send(frames[1], { tsugumi: 'state', state: 'ready', requestId: id3, image: png('third') })
    await third
    assert.deepEqual(settled, { image: png('third'), pixelBytes: 32 })
    renderer.close()
    assert.equal(listeners.size, 0)
  } finally {
    win.addEventListener = before.add; win.removeEventListener = before.remove
    Object.assign(page.document as object, { createElement: before.create, body: before.body })
  }
}

console.log('figure previews: display modes, held pictures, rapid revisions, view changes and late replies passed')
