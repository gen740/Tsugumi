import { projectFixture } from './documentFixture'
import { cardIds } from './cardIds'
// A junction is where one output's wires split on the canvas: layout only.
// Arrange places one on an output with three or more inputs, a person moves it
// or adds and removes it from the Card's menu, and the connections never change.
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import type { Workspace } from '../src/model/workspace'
import { arr, get, parse, serialize } from '../shared/json'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { CanvasController } = await import('../src/model/canvas')
const { ProjectSession } = await import('../src/model/session')
const { WorkflowTab } = await import('../src/model/workspace')
const { prefs } = await import('../src/model/prefs')
const { cardMenu, GraphCanvas } = await import('../src/ui/canvas')
const { layoutView } = await import('../src/model/documents')
const { layoutFixture } = await import('./documentFixture')
const originalGrid = prefs.showGrid
prefs.set('showGrid', false)

const s = cardIds(new ProjectSession('/p', 'k', 'p'))
s.project = parse(projectFixture({ name: 't', canvas: { cards: [], connections: [] } }))
s.layout = parse('{}')
s.setFlow('topToBottom')
const source = await s.addCard('collect', [], { x: 0, y: 0 }, 'source')
const targets = [await s.addCard('collect', [], { x: 900, y: 40 }, 'a'),
  await s.addCard('collect', [], { x: -700, y: 900 }, 'b'),
  await s.addCard('collect', [], { x: 300, y: 1500 }, 'c')]
const pair = await s.addCard('collect', [], { x: 2000, y: 0 }, 'pair')
const pairTargets = [await s.addCard('collect', [], { x: 2000, y: 600 }, 'p1'), await s.addCard('collect', [], { x: 2400, y: 600 }, 'p2')]
for (const t of targets) await s.connect([source, 'out'], [t, 'in'], [])
for (const t of pairTargets) await s.connect([pair, 'out'], [t, 'in'], [])
await s.save()
const connections = serialize(s.connections([]))
const tab = new WorkflowTab([])
const cv = new CanvasController(s, tab)
cv.size = { width: 800, height: 600 }
cv.setCamera({ x: 5000, y: 5000, zoom: 1 })
assert.equal(s.junction(source, 'out'), undefined, 'no junction before Arrange')
assert.deepEqual(s.junctionOutputs(source), ['out'])
assert.deepEqual(s.junctionOutputs(pair), [], 'two inputs are not enough')

{
  // Arrange places the junction of the output with three inputs, in one step of undo,
  // and the view then shows the arranged graph.
  const undos = s.undoStack.length
  await cv.arrange()
  assert.equal(s.undoStack.length, undos + 1)
  const j = s.junction(source, 'out')
  assert.ok(j, 'Arrange placed a junction')
  assert.equal(s.junction(pair, 'out'), undefined)
  assert.equal(serialize(s.connections([])), connections, 'the connections are unchanged')
  const scene = cv.scene()
  const port = scene.portWorld(source, 'out', true)
  assert.ok(j.y > port.y, 'below the output, along the flow')
  const near = Math.min(...targets.map((t) => scene.portWorld(t, 'in', false).y))
  assert.ok(j.y < near, 'before its nearest input')
  // One trunk from the output to the junction; the branches start at the junction.
  const trunks = scene.wires.filter((w) => w.trunk)
  assert.equal(trunks.length, 1)
  assert.deepEqual(trunks[0].pts[0], port)
  assert.deepEqual(trunks[0].pts.at(-1), j)
  const branches = scene.wires.filter((w) => w.connection !== undefined && w.pts[0].x === j.x && w.pts[0].y === j.y)
  assert.equal(branches.length, 3)
  assert.equal(scene.wires.filter((w) => w.connection !== undefined).length, 5, 'still one wire per connection')
  // The view fits the arranged graph.
  const bounds = scene.bounds()!
  const c = cv.camera
  assert.ok(bounds.x * c.zoom + c.x >= 0 && (bounds.x + bounds.width) * c.zoom + c.x <= cv.size.width, 'the arranged graph is in view')
  // The canvas draws it as a dot that is no Card.
  const { error } = console
  console.error = () => {}
  const html = renderToStaticMarkup(<GraphCanvas workspace={{} as Workspace} session={s} tab={tab} />)
  console.error = error
  assert.ok(html.includes(`data-junction="${source}.out"`))
  assert.ok(!html.includes(`data-junction="${pair}.out"`))
}

{
  // Dragging moves it (layout only, one undo step).
  const was = s.junction(source, 'out')!
  s.setJunction(source, 'out', { x: was.x + 40, y: was.y + 20 })
  await s.save()
  assert.deepEqual(s.junction(source, 'out'), { x: was.x + 40, y: was.y + 20 })
  assert.equal(serialize(s.connections([])), connections)
  await s.undo()
  assert.deepEqual(s.junction(source, 'out'), was)
}

{
  // The Card's menu removes and adds it again.
  const label = (k: string) => (cardMenu({} as Workspace, s, tab, source) as { label?: string; action?: () => void }[])
    .find((item) => item && typeof item === 'object' && item.label?.includes(k))
  const remove = label('分岐点を外す')
  assert.ok(remove?.action)
  remove.action()
  await s.save()
  assert.equal(s.junction(source, 'out'), undefined)
  assert.equal(cv.scene().wires.filter((w) => w.trunk).length, 0, 'without one, wires go directly')
  const add = label('分岐点を置く')
  assert.ok(add?.action)
  add.action()
  await s.save()
  const j = s.junction(source, 'out')!
  assert.ok(j && j.y > cv.scene().portWorld(source, 'out', true).y, 'placed as Arrange would')
  assert.equal(serialize(s.connections([])), connections)
  assert.ok(label('分岐点を外す'), 'the menu offers to remove it again')
  assert.equal((cardMenu({} as Workspace, s, tab, pair) as { label?: string }[]).some((i) => i && typeof i === 'object' && i.label?.includes('分岐点')), false)
}

{
  // The layout keeps junctions through layout.bin.
  const view = parse(serialize(s.layout))
  const { bytes, notes } = layoutFixture(serialize(view))
  const back = layoutView(Uint8Array.from(bytes), notes)
  assert.equal(serialize(back), serialize(view))
  assert.equal((arr(get(back, 'cards', source, 'junctions')) ?? []).length, 1)
}

prefs.set('showGrid', originalGrid)
console.log('junction behavior: passed')
