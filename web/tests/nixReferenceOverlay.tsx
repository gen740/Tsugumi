// The Nix reference overlay as the canvas uses it: what a saved source change
// from the core does to it, and what the toggle, a click on a link and
// hovering a Card show.
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { parse } from '../src/shared/json'
import * as R from '../src/gen/tsugumi/rpc'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')
const { WorkflowTab } = await import('../src/model/workspace')
const { activeNixLinks, nixLinks, nixReferences } = await import('../src/model/nixReferences')
const { NixReferenceControls, NixReferenceList } = await import('../src/ui/nixReferences')

type Ref = { target: string[]; name: string; valid?: boolean }
const resolved = (id: string, refs: Ref[] = []) => ({
  card: { segments: id.split('/') }, inputs: [], outputs: [], diagnostics: [],
  nixReferences: refs.map(r => ({ target: { segments: r.target }, name: r.name, valid: r.valid ?? true })),
}) as unknown as R.ResolvedCardT

const session = new ProjectSession('/p', 'k', 'p')
const project = '{"name":"t","canvas":{"cards":[{"id":"source"},{"id":"analysis"},{"id":"report"},{"id":"other"}],"connections":[]}}'
session.project = parse(project)
session.confirmedProject = parse(project)
session.layout = parse('{}')
session.confirmedLayout = parse('{}')
session.rev = 1n
const inner = session as unknown as { applyDelta(d: R.DocumentDeltaT): void; applyResolution(r: R.ResolvedCardT[], d: unknown[]): void }
const delta = (resolution: R.ResolvedCardT[]) => {
  const d = Object.assign(new R.DocumentDeltaT(), { baseRev: session.rev, rev: session.rev + 1n, resolution, diagnostics: [] })
  inner.applyDelta(d)
}
const cards = ['source', 'analysis', 'report', 'other']
const all = (refs: Record<string, Ref[]>) => cards.map(c => resolved(c, refs[c]))
const shown = () => nixReferences(session).map(r => [r.source, r.target, r.expression, r.valid])

// A saved default.nix arrives as the resolution of a document delta: the
// overlay is what that resolution says, with nothing kept from before.
inner.applyResolution(all({}), [])
assert.deepEqual(shown(), [])
delta(all({ analysis: [{ target: ['source'], name: 'source' }] }))
assert.deepEqual(shown(), [['analysis', 'source', 'inputs.source.export', true]])
delta(all({
  analysis: [{ target: ['source'], name: 'source' }],
  report: [{ target: ['analysis'], name: 'analysis' }, { target: ['missing'], name: 'missing', valid: false }],
}))
assert.deepEqual(shown(), [
  ['analysis', 'source', 'inputs.source.export', true],
  ['report', 'analysis', 'inputs.analysis.export', true],
  ['report', 'missing', 'inputs.missing.export', false],
])
delta(all({ report: [{ target: ['analysis'], name: 'analysis' }] }))
assert.deepEqual(shown(), [['report', 'analysis', 'inputs.analysis.export', true]], 'a removed reference leaves with the delta that removed it')

// What the canvas draws, and the three interactions on it.
delta(all({
  analysis: [{ target: ['source'], name: 'source' }],
  report: [{ target: ['analysis'], name: 'analysis' }, { target: ['missing'], name: 'missing', valid: false }],
}))
const tab = new WorkflowTab([])
const visible = new Set(cards)
const links = () => tab.showNixReferences ? nixLinks(nixReferences(session), tab.scope, visible) : []
const active = () => activeNixLinks(links(), tab.hoveredNixCard, tab.selectedNixReference)
const ends = (l: { source: string; target: string }[]) => l.map(r => `${r.source}>${r.target}`)

assert.deepEqual(links(), [], 'nothing is drawn until the layer is turned on')
tab.hoverNixCard('analysis')
assert.equal(tab.hoveredNixCard, undefined, 'hovering marks nothing while the layer is off')
tab.toggleNixReferences()
assert.deepEqual(ends(links()), ['analysis>source', 'report>analysis'], 'a reference to a Card that is not on the canvas has no link')
assert.deepEqual(active(), [])

// Hover: every link of the Card, and the Cards at their other ends.
tab.hoverNixCard('analysis')
assert.deepEqual(ends(active()), ['analysis>source', 'report>analysis'])
assert.deepEqual([...new Set(active().flatMap(r => [r.source, r.target]))].sort(), ['analysis', 'report', 'source'])
tab.hoverNixCard('source')
assert.deepEqual(ends(active()), ['analysis>source'], 'the unrelated link is not highlighted')
tab.hoverNixCard('other')
assert.deepEqual(active(), [], 'a Card without references highlights nothing')
const version = tab.version
tab.hoverNixCard('other')
assert.equal(tab.version, version, 'staying on the same Card redraws nothing')
tab.hoverNixCard(undefined)

// Click: the link is the selected row, and the list opens on it.
const picked = links()[1]
tab.selectNixReference(picked.id)
assert.equal(tab.showNixList, true)
assert.deepEqual(ends(active()), ['report>analysis'])
const html = renderToStaticMarkup(<NixReferenceList session={session} tab={tab} />)
const rows = html.split('<code').slice(0, -1).map(before => before.slice(before.lastIndexOf('border:1px solid')))
assert.equal(rows.length, 3, 'the list keeps the invalid reference the canvas cannot draw')
assert.deepEqual(rows.map(r => r.startsWith('border:1px solid var(--purple)')), [false, true, false], 'only the picked link is the selected row')
assert.ok(html.includes('参照を解決できません'), 'the invalid reference says so')

// The toggle says what the layer is, pressed or not.
tab.showNixList = false
const on = renderToStaticMarkup(<NixReferenceControls session={session} tab={tab} />)
assert.ok(on.includes('Nix 参照（記述どおり）'), 'these are the references as written')
assert.ok(on.includes('別名や計算で決まる参照は含みません'), 'and what that leaves out')

// Turning the layer off drops the hover with it.
tab.hoverNixCard('analysis')
tab.toggleNixReferences()
const off = renderToStaticMarkup(<NixReferenceControls session={session} tab={tab} />)
assert.notEqual(on, off, 'the toggle shows whether the layer is on')
assert.equal(tab.hoveredNixCard, undefined)
assert.deepEqual(links(), [])

// Inside a Card in Card only that level's Cards meet.
const nested = new WorkflowTab(['child'])
nested.toggleNixReferences()
const inside = nixLinks([{ id: 'x', source: 'child/sink', target: 'child/inner/deep', expression: '', valid: true },
  { id: 'y', source: 'sink', target: 'child/inner', expression: '', valid: true }], nested.scope, new Set(['child/sink', 'child/inner']))
assert.deepEqual(ends(inside), ['child/sink>child/inner'])

console.log('Nix reference overlay: passed')
