// The Nix reference overlay as the canvas uses it: what a saved source change
// from the core does to it, and what the toggle, a click on a link and
// hovering a Card show.
import assert from 'node:assert/strict'
import TestRenderer from 'react-test-renderer'
import * as R from '../shared/gen/tsugumi/rpc'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')
const { WorkflowTab } = await import('../src/model/workspace')
const { activeNixLinks, nixLinks, nixReferences } = await import('../src/model/nixReferences')
const { NixReferenceControls, NixReferenceList } = await import('../src/ui/nixReferences')

type Ref = { target: string[]; name: string }
const resolved = (id: string, refs: Ref[] = []) => ({
  card: { segments: id.split('/') }, inputs: [], outputs: [], diagnostics: [],
  nixReferences: refs.map(r => ({ target: { segments: r.target }, name: r.name })),
}) as unknown as R.ResolvedCardT

const session = new ProjectSession('/p', 'k', 'p')
const project = '{"name":"t","canvas":{"cards":[{"id":"source"},{"id":"analysis"},{"id":"report"},{"id":"other"}],"connections":[]}}'
const inner = session as unknown as { applyDelta(d: R.DocumentDeltaT): void; apply(snapshot: R.ProjectSnapshotT, reset: boolean): void }
const delta = (resolution: R.ResolvedCardT[]) => {
  const d = Object.assign(new R.DocumentDeltaT(), { baseRev: session.rev, rev: session.rev + 1n, resolution, diagnostics: [] })
  inner.applyDelta(d)
}
const cards = ['source', 'analysis', 'report', 'other']
const all = (refs: Record<string, Ref[]>) => cards.map(c => resolved(c, refs[c]))
const shown = () => new Map(nixReferences(session).map(r => [r.target, r.source]))

// Wiring changes arrive as the resolution of a document delta: the
// overlay is what that resolution says, with nothing kept from before.
inner.apply(Object.assign(new R.ProjectSnapshotT(), { rev: 1n, projectJson: project, layout: [], resolution: all({}) }))
assert.deepEqual(shown(), new Map())
delta(all({ analysis: [{ target: ['source'], name: 'source' }] }))
assert.deepEqual(shown(), new Map([['source', 'analysis']]))
delta(all({
  analysis: [{ target: ['source'], name: 'source' }],
  report: [{ target: ['analysis'], name: 'analysis' }, { target: ['missing'], name: 'missing' }],
}))
assert.deepEqual(shown(), new Map([
  ['source', 'analysis'],
  ['analysis', 'report'],
  ['missing', 'report'],
]))
delta(all({ report: [{ target: ['analysis'], name: 'analysis' }] }))
assert.deepEqual(shown(), new Map([['analysis', 'report']]), 'a removed reference leaves with the delta that removed it')

// What the canvas draws, and the three interactions on it.
delta(all({
  analysis: [{ target: ['source'], name: 'source' }],
  report: [{ target: ['analysis'], name: 'analysis' }, { target: ['missing'], name: 'missing' }],
}))
const tab = new WorkflowTab([])
const visible = new Set(cards)
const links = () => tab.showNixReferences ? nixLinks(nixReferences(session), tab.scope, visible) : []
const active = () => activeNixLinks(links(), tab.hoveredNixCard, tab.selectedNixReference)
const ends = (l: { source: string; target: string }[]) => new Set(l.map(r => `${r.source}>${r.target}`))

assert.deepEqual(links(), [], 'nothing is drawn until the layer is turned on')
tab.hoverNixCard('analysis')
assert.equal(tab.hoveredNixCard, undefined, 'hovering marks nothing while the layer is off')
tab.toggleNixReferences()
assert.deepEqual(ends(links()), new Set(['analysis>source', 'report>analysis']), 'a reference to a Card that is not on the canvas has no link')
assert.deepEqual(active(), [])

// Hover: every link of the Card, and the Cards at their other ends.
tab.hoverNixCard('analysis')
assert.deepEqual(ends(active()), new Set(['analysis>source', 'report>analysis']))
assert.deepEqual(new Set(active().flatMap(r => [r.source, r.target])), new Set(['analysis', 'report', 'source']))
tab.hoverNixCard('source')
assert.deepEqual(ends(active()), new Set(['analysis>source']), 'the unrelated link is not highlighted')
tab.hoverNixCard('other')
assert.deepEqual(active(), [], 'a Card without references highlights nothing')
const version = tab.version
tab.hoverNixCard('other')
assert.equal(tab.version, version, 'staying on the same Card redraws nothing')
tab.hoverNixCard(undefined)

// Click: the link is the selected row, and the list opens on it.
const picked = links().find(link => link.source === 'report' && link.target === 'analysis')
assert.ok(picked)
tab.selectNixReference(picked.id)
assert.equal(tab.showNixList, true)
assert.deepEqual(ends(active()), new Set(['report>analysis']))
const rendered = TestRenderer.create(<NixReferenceList session={session} tab={tab} />)
const target = rendered.root.findAllByType('button').find(button =>
  button.findAllByType('span').some(label => label.props.children === 'missing'))
assert.equal(target?.props.disabled, true, 'an unresolved target remains listed without navigation')
rendered.unmount()

tab.showNixList = false
const controls = () => TestRenderer.create(<NixReferenceControls session={session} tab={tab} />)
const on = controls()
assert.equal(on.root.findAllByType('button').find(button => button.props['aria-pressed'] !== undefined)?.props['aria-pressed'], true)
assert.equal(on.root.findAllByType('span').some(span => typeof span.props.title === 'string'), false)
on.unmount()

// Turning the layer off drops the hover with it.
tab.hoverNixCard('analysis')
tab.toggleNixReferences()
const off = controls()
assert.equal(off.root.findAllByType('button').find(button => button.props['aria-pressed'] !== undefined)?.props['aria-pressed'], false)
off.unmount()
assert.equal(tab.hoveredNixCard, undefined)
assert.deepEqual(links(), [])

// Inside a Card in Card only that level's Cards meet.
const nested = new WorkflowTab(['child'])
nested.toggleNixReferences()
// Turned on where nothing is drawn, the bar says so (and how many the list has).
const empty = TestRenderer.create(<NixReferenceControls session={session} tab={nested} />)
const notice = empty.root.findAllByType('span').find(span => typeof span.props.title === 'string')
assert.ok(notice, 'an empty overlay has an explanation')
assert.ok(String(notice.props.children).includes(String(nixReferences(session).length)))
empty.unmount()
const inside = nixLinks([{ id: 'x', source: 'child/sink', target: 'child/inner/deep', expression: '' },
  { id: 'y', source: 'sink', target: 'child/inner', expression: '' }], nested.scope, new Set(['child/sink', 'child/inner']))
assert.deepEqual(ends(inside), new Set(['child/sink>child/inner']))

console.log('Nix reference overlay: passed')
