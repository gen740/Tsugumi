// An ordinary Card's face: its name (two lines at most), one status line and
// the stage circles on the right. Its size is the same in every state and for
// any name; the rest is on hover. The Mac app checks the same (UILogicTests:
// aCardsFaceKeepsItsSizeWhateverItsState, aCardsStatusLineSaysItsStateInOneLine,
// theHoverHasWhatTheFaceLeavesOut).
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { projectFixture } from './documentFixture'
import { addCodeCard, cardIds } from './cardIds'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { CardGeometry, Stage, STAGE_INFO } = await import('../src/model/canvasKit')
const { ProjectSession, cardState, formatShortDuration, relativeAge } = await import('../src/model/session')
const { CardNodeView, statusLine } = await import('../src/ui/cardNode')
const { cardHoverContent, hoverContent } = await import('../src/ui/canvas')
const { D } = await import('../src/wire/rpc')
const { parse } = await import('../shared/json')
type Data = import('../src/model/canvasKit').CardNodeData

const card = (title = 'x'): Data => ({
  title, kind: 'code', summary: 'what it does', inputs: [{ name: 'a' }], outputs: [{ name: 'b' }],
  invocation: 0n, runContinues: false, errors: [], warnings: [], collapsed: false,
})
const stages = (build: string, run: string, validate: string) => ({
  items: [{ stage: Stage.build, status: build }, { stage: Stage.execute, status: run }, { stage: Stage.validate, status: validate }],
  workerReused: false,
}) as Data['stage']

const running: Data = { ...card(), state: D.CardState.running, invocation: 1n, stage: stages('done', 'running', 'pending') }
const queued: Data = { ...card(), state: D.CardState.preparing, invocation: 1n, activity: 'waiting for a worker' }
const now = Date.now()
const succeeded: Data = { ...card(), state: D.CardState.succeeded, invocation: 3n, endedAt: now - 180_000, seconds: 4, stage: stages('cached', 'done', 'done') }
const failed: Data = {
  ...card(), state: D.CardState.failed, invocation: 1n, failure: '\nerror: no such file\n  at main.py:3', stage: stages('done', 'failed', 'notRun'),
}
const long = card('a long name of a Card '.repeat(6))

{
  // The same size whatever the state and the name.
  for (const flow of ['topToBottom', 'leftToRight'] as const) {
    const size = new CardGeometry(card(), flow).size
    for (const d of [running, queued, succeeded, failed, long]) assert.deepEqual(new CardGeometry(d, flow).size, size, `${d.title} ${flow}`)
  }
  const g = new CardGeometry(card(), 'topToBottom')
  assert.ok(g.face && !g.summary, 'the description is on hover')
}

{
  // One status line per state.
  assert.equal(statusLine(card()).text, '未実行')
  assert.equal(statusLine(running).text, STAGE_INFO[Stage.execute].active)
  assert.equal(statusLine(running).color, cardState(D.CardState.running).color)
  assert.equal(statusLine(queued).text, 'waiting for a worker')
  assert.equal(statusLine(succeeded, now).text, `${cardState(D.CardState.succeeded).label} ×3 · ${relativeAge(now - 180_000, now)} · 4.0 s`)
  assert.equal(statusLine(succeeded, now).color, 'secondary')
  assert.equal(statusLine(failed).text, STAGE_INFO[Stage.execute].name + ': no such file')
  assert.equal(statusLine(failed).color, cardState(D.CardState.failed).color)
  assert.deepEqual([formatShortDuration(4), formatShortDuration(192), formatShortDuration(3723)], ['4.0 s', '3:12', '1:02:03'])
}

{
  // The face: name, status line and three circles; no description.
  const html = renderToStaticMarkup(<CardNodeView data={failed} flow="topToBottom" />)
  assert.ok(html.includes('cv-face') && html.includes('no such file'))
  assert.equal(html.match(/class="cv-stage"/g)?.length, 3)
  // Read out to assistive technology, not drawn.
  assert.ok(!html.includes('>what it does<') && html.includes('aria-label="Card x'))
  // A Card in Card keeps its kind's symbol instead of circles.
  const nested = renderToStaticMarkup(<CardNodeView data={{ ...card('nested'), kind: 'subgraph', symbol: 'link' }} flow="topToBottom" />)
  assert.ok(nested.includes('cv-face') && !nested.includes('cv-stage'))
}

{
  // The hover has the description, the stages and the whole failure with a way to the log.
  const s = cardIds(new ProjectSession('/p', 'k', 'p'))
  s.project = parse(projectFixture({ name: 't', canvas: { cards: [], connections: [] } }))
  s.layout = parse('{}')
  s.connected = true
  const k = await addCodeCard(s, { x: 0, y: 0 })
  s.updateMeta(k, (m) => m.set('description', 'what it does'))
  await s.save()
  const idle = hoverContent(s, [], { kind: 'card', k })
  assert.deepEqual(idle.lines, ['what it does'])
  assert.equal(idle.openLog, undefined)
  assert.equal(idle.failure, undefined)
  const hover = cardHoverContent(failed, ['what it does'], '0123456789', () => {})
  assert.deepEqual(hover.lines, ['what it does'])
  assert.deepEqual(hover.stages?.map((i) => i.stage), [Stage.build, Stage.execute, Stage.validate])
  assert.equal(hover.failure, failed.failure)
  assert.ok(hover.openLog)
  assert.deepEqual(hover.problems, [])
}
console.log('card face: passed')
