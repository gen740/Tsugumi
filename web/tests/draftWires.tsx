// A wire to or from a Draft is dotted, whatever else it shows, until the Draft
// has code. The Mac app checks the same (UILogicTests: draftWiresAreDotted).
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { get, parse, str } from '../src/shared/json'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { isDraftWire, NEW_PORT } = await import('../src/model/canvasKit')
const { Scene } = await import('../src/model/canvas')
const { ProjectSession } = await import('../src/model/session')
const { WireLayer } = await import('../src/ui/cardNode')

const session = () => {
  const s = new ProjectSession('/p', 'k', 'p')
  s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  s.layout = parse('{}')
  s.scheduleSave = () => {} // no tsugumid here
  return s
}
/** Dotted or not, by wire, as a new Scene (one per update of the canvas) sees them. */
const dotted = (s: InstanceType<typeof ProjectSession>, scope: string[] = [], selected?: number) => {
  const conns = s.connections(scope)
  return Object.fromEntries(new Scene(s, scope, selected).wires.filter((w) => w.connection !== undefined).map((w) => {
    const c = conns[w.connection!]
    return [`${str(get(c, 'from', 'card'))}>${str(get(c, 'to', 'card'))}`, !!w.dotted]
  }))
}

{
  const kinds: Record<string, 'draft' | 'code' | 'subgraph'> = { d: 'draft', c: 'code', s: 'subgraph' }
  const kind = (id: string) => kinds[id]
  assert.ok(isDraftWire(kind, 'd', 'c'), 'from a Draft')
  assert.ok(isDraftWire(kind, 'c', 'd'), 'to a Draft')
  assert.ok(isDraftWire(kind, 'd', 'd'), 'between Drafts')
  assert.ok(isDraftWire(kind, 'd'), 'from a Draft to the bar of a Card in Card')
  assert.ok(!isDraftWire(kind, 'c', 's'), 'between Cards with code')
  assert.ok(!isDraftWire(kind, 'c'), 'from a Card with code to the bar')
}

{
  const s = session()
  const start = s.addCard('trigger', [], { x: 0, y: 0 }, 'start')
  const log = s.addCard('python', [], { x: 600, y: 0 }, 'log')
  const draft = s.addCard('draft', [], { x: 300, y: 0 }, 'plan')
  s.connect([start, 'out'], [log, 'in'], [])
  assert.deepEqual(dotted(s), { 'start>log': false }, 'Cards with code keep their solid wire')

  s.disconnect(log, true, [])
  s.connect([start, 'out'], [draft, NEW_PORT], [])
  s.connect([draft, NEW_PORT], [log, 'in'], [])
  assert.deepEqual(dotted(s), { 'start>plan': true, 'plan>log': true })

  // The style says what the wire is doing; the dots stay with it.
  const scene = new Scene(s, [], 0)
  assert.equal(scene.wires.find((w) => w.connection === 0)?.style, 'selected')
  assert.equal(dotted(s, [], 0)['start>plan'], true, 'selected')
  s.cardStates.set(start, { stages: [], sent: new Set(['out']), sentInRun: new Set(['out']) } as never)
  assert.equal(new Scene(s, []).wires.find((w) => w.connection === 0)?.style, 'carried')
  assert.equal(dotted(s)['start>plan'], true, 'carried')
  s.cardStates.clear()

  // Code written for the Draft: its wires are solid again, without any other change.
  s.updateCard(draft, (c) => {
    c.set('template', undefined)
    c.set('language', 'python')
  })
  assert.equal(s.kind(draft), 'code')
  assert.deepEqual(dotted(s), { 'start>plan': false, 'plan>log': false })
}

{
  // In a Card in Card: its bars' wires to an inner Draft are dotted, the wires of the Card itself are not.
  const s = session()
  const start = s.addCard('trigger', [], { x: 0, y: 0 }, 'start')
  const log = s.addCard('python', [], { x: 600, y: 0 }, 'log')
  const draft = s.addCard('draft', [], { x: 300, y: 0 }, 'plan')
  s.connect([start, 'out'], [draft, NEW_PORT], [])
  s.connect([draft, NEW_PORT], [log, 'in'], [])
  const outer = await s.makeSubgraph(new Set([draft]), [])
  assert.ok(outer)
  assert.equal(s.kind(outer), 'subgraph')
  assert.ok(Object.values(dotted(s)).length > 0)
  assert.ok(Object.values(dotted(s)).every((d) => !d), 'a Card in Card is not a Draft')
  const inside = new Scene(s, [outer])
  assert.ok(inside.boundary)
  const bars = inside.wires.filter((w) => w.connection === undefined && w.style !== 'display')
  assert.equal(bars.length, 2)
  assert.ok(bars.every((w) => w.dotted))
}

{
  // Drawn: the dots replace the style's own dashes, and move while the value does.
  const html = (style: 'normal' | 'carried' | 'active' | 'selected' | 'display', dotted?: boolean, animate = false) => renderToStaticMarkup(
    <WireLayer wires={[{ d: 'M0,0L9,9', style, dotted }]} animate={animate} width={10} height={10} />)
  const dash = (markup: string) => /stroke-dasharray="([^"]*)"/.exec(markup)?.[1]
  const dots = dash(html('normal', true))!
  const [on, off] = dots.split(' ').map(Number)
  assert.equal(on, 0, 'round dots')
  assert.ok(off > 0)
  const period = 8 + 6 // the active wires' dashes, what cv-dash moves by
  assert.ok(Number.isInteger(Number((period / off).toFixed(6))), 'the dots flow without a jump')
  for (const style of ['carried', 'active'] as const) assert.equal(dash(html(style, true)), dots, style)
  assert.ok(Number(dash(html('selected', true))!.split(' ')[1]) > off, 'a thicker selected wire keeps its dots apart')
  const width = (dotted: boolean) => Number(/stroke-width="([^"]*)"/.exec(renderToStaticMarkup(
    <WireLayer wires={[{ d: 'M0,0L9,9', style: 'normal', dotted }]} zoom={0.2} width={10} height={10} />))?.[1])
  assert.equal(width(false), 1)
  assert.ok(width(true) >= 2, 'the dots can be seen on a Canvas zoomed out')
  for (const style of ['normal', 'carried', 'selected'] as const) assert.equal(dash(html(style)), undefined, `${style}, not a Draft's`)
  assert.equal(dash(html('active', false, true)), '8 6', 'the moving dashes of other wires are as they were')
  assert.ok(html('active', true, true).includes('cv-wire-moving'))
  assert.equal(dash(html('display')), '5 4', 'the link to a result frame is as it was')
}
console.log('draft wires: passed')
