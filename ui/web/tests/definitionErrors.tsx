import { cardFixture, projectFixture } from './documentFixture'
// A Card whose _card.json has no type or an unknown one is drawn as a failure with
// the core's diagnostic, no ports and no run. The Mac app checks the same
// (UILogicTests: cardsWithoutAKnownTypeAreDefinitionErrors).
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import * as R from '../shared/gen/tsugumi/rpc'
import * as DT from '../shared/gen/tsugumi/data'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession, cardKindNamed, definitionKind } = await import('../src/model/session')
const { CardGeometry } = await import('../src/model/canvasKit')
const { CardNodeView, idleText } = await import('../src/ui/cardNode')
const { miniMapLayout } = await import('../src/ui/versions')
const { parse } = await import('../shared/json')

const missing = 'definitions/old/_card.json: "type" is required (one of code, draft, trigger, merge, collect, subgraph)'
const diagnostic = (id: string, message: string) => Object.assign(new DT.DiagnosticT(), {
  severity: DT.Severity.error, code: 'invalid_document', message, card: Object.assign(new DT.CardPathT(), { segments: [id] }),
  path: `definitions/${id}/_card.json`,
})
const resolved = (id: string, diagnostics: DT.DiagnosticT[] = []) =>
  ({ card: { segments: [id] }, inputs: [], outputs: [Object.assign(new DT.PortT(), { name: 'out' })], diagnostics, nixReferences: [] }) as unknown as R.ResolvedCardT
const definition = (id: string, card: string) => Object.assign(new R.DefinitionT(), { id, cardJson: card })

const s = new ProjectSession('/p', 'k', 'p')
const inner = s as unknown as { apply(snapshot: R.ProjectSnapshotT, reset: boolean): void }
inner.apply(Object.assign(new R.ProjectSnapshotT(), {
  rev: 1n, layout: [],
  projectJson: projectFixture({"name":"t","canvas":{"cards":[{"id":"old"},{"id":"future"},{"id":"plan"},{"id":"step"}],"connections":[]}}),
  definitions: [
    definition('old', '{"inputs":[{"name":"in"}]}'),
    definition('future', '{"type":"something new","inputs":[{"name":"in"}]}'),
    definition('plan', cardFixture('draft')),
    definition('step', cardFixture('code')),
  ],
  resolution: [resolved('old', [diagnostic('old', missing)]), resolved('future'), resolved('plan'), resolved('step')],
}), true)

assert.equal(s.kind('old'), 'invalid')
assert.equal(s.kind('future'), 'invalid')
assert.equal(s.kind('plan'), 'draft', 'a Draft stays a Draft')
assert.equal(s.kind('step'), 'code')
assert.equal(definitionKind(undefined), 'invalid', 'a Card without a definition')
assert.equal(definitionKind(parse('{"type":"subgraph","symbol":"link"}')), 'subgraph')

for (const id of ['old', 'future']) {
  const d = s.nodeData(id)
  assert.equal(d.kind, 'invalid')
  assert.deepEqual([d.inputs, d.outputs], [[], []], `${id}: no ports, declared or resolved`)
  const g = new CardGeometry(d, 'topToBottom')
  assert.deepEqual([g.inputs, g.outputs, g.draft, g.stages], [[], [], false, false])
  assert.equal(g.newPort(false), undefined, 'nothing to draw a wire from')
  assert.equal(idleText(d), '定義エラー')
}
// The core's diagnostic (file and message) is what the Card says.
const old = s.nodeData('old')
assert.equal(old.summary, missing)
assert.deepEqual(old.errors, [missing])
assert.ok(new CardGeometry(old, 'topToBottom').summary, 'the diagnostic has its row')
assert.equal(s.nodeData('future').summary, 'Card の定義を読めません', 'without a diagnostic yet')

// Drawn as a failure, not as a Draft's dashed outline.
const html = renderToStaticMarkup(<CardNodeView data={old} flow="topToBottom" />)
assert.ok(html.includes('定義エラー') && html.includes(missing.replace(/"/g, '&quot;')))
assert.ok(html.includes('var(--red)'))
assert.ok(!html.includes('stroke-dasharray'))
assert.ok(!html.includes('cv-port'))

// A version from before `type` (or one this build cannot read) has no kind.
assert.equal(cardKindNamed(''), 'invalid')
assert.equal(cardKindNamed('something new'), 'invalid')
const map = miniMapLayout([{ id: 'old', x: 0, y: 0, kind: cardKindNamed(''), fan: 0, collapsed: false },
  { id: 'step', x: 300, y: 0, kind: 'code', fan: 0, collapsed: false }], 'leftToRight', 284, 132)
assert.deepEqual(map.cards.map((c) => c.invalid), [true, false])

console.log('definition errors: passed')
