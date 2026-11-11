// The Agent's bird beside a Card goes with the canvas's zoom, as the helpers' birds
// do: at 39% a bird of 90 points stood over the Cards it was working on.
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import type { Workspace } from '../src/model/workspace'
import { parse } from '../src/shared/json'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { canvasFor } = await import('../src/model/canvas')
const { BIRD, perchScale } = await import('../src/model/helperBirds')
const { ProjectSession } = await import('../src/model/session')
const { WorkflowTab } = await import('../src/model/workspace')
const { GraphCanvas } = await import('../src/ui/canvas')

// With the zoom when zoomed out; never too small to see, never past its own size.
assert.equal(perchScale('0.7'), 0.7)
assert.equal(perchScale('0.39'), 0.45)
assert.equal(perchScale('0.2'), 0.45)
assert.equal(perchScale('1'), 1)
assert.equal(perchScale('3'), 1)
// What the guide points at off the canvas has no zoom: the bird is as it is.
assert.equal(perchScale(undefined), 1)

// At 39% it is half its size, and still larger than a helper there.
{
  const lead = 90 * perchScale('0.39')
  const helper = 680 * BIRD.scale * 0.39 * BIRD.helperScale
  assert.ok(lead < 45)
  assert.ok(lead > helper)
}

// The canvas around a Card says its zoom, where the guide looks for it from the Card.
{
  const s = new ProjectSession('/p', 'k', 'p')
  s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  s.layout = parse('{}')
  s.scheduleSave = () => {} // no tsugumid here
  const k = s.addCard('trigger', [], { x: 0, y: 0 }, 'start')
  const tab = new WorkflowTab([])
  canvasFor(s, tab).setCamera({ x: 0, y: 0, zoom: 0.39 })
  // React warns that the canvas's layout effects do not run here: they need not.
  const { error } = console
  console.error = () => {}
  const html = renderToStaticMarkup(<GraphCanvas workspace={{} as Workspace} session={s} tab={tab} />)
  console.error = error
  const root = /^<div[^>]*>/.exec(html)![0]
  assert.match(root, /data-canvas-zoom="0\.39"/)
  assert.ok(html.includes(`data-guide="card:${k}"`), 'the Card is inside it')
}

console.log('perch scale behavior: passed')
