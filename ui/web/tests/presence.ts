import assert from 'node:assert/strict'
import { connectionKey, SavePresence, SAVE_CUE_SECONDS } from '../src/model/presence'
import { parse } from '../shared/json'
import { canvasBirdBox, HelperTeam } from '../src/model/helperBirds'

const wires = Array.from({ length: 15 }, (_, i) => parse(JSON.stringify({ from: { card: 'a', port: 'out' }, to: { card: `b${i}`, port: 'in' } })))
const p = new SavePresence()
p.saved(['a', 'b', 'c'], [], wires, 100)
assert.equal(p.step?.at, 100)
assert.equal(p.step?.cards.size, 3)
assert.equal(p.step?.wires.size, 15)
assert.ok(Math.abs(p.progress(100.12)! - 0.5) < 1e-9)
for (const w of wires) assert.ok(p.step?.wires.has(connectionKey(w)))
assert.equal(p.progress(100 + SAVE_CUE_SECONDS), undefined)
// Run (or any newer event) completes the entire step immediately.
p.finish()
assert.equal(p.step, undefined)
assert.equal(p.progress(100.13), undefined)
for (let i = 0; i < 1000; i++) p.saved([`c${i}`], [], wires, 101)
assert.deepEqual([...p.step!.cards], ['c999'])
p.saved(['a'], wires, wires, 102)
assert.equal(p.step!.wires.size, 0, 'source edits do not reconnect existing wires')

// Every helper follows latest work, without waiting for its old flights.
const team = new HelperTeam(() => undefined)
team.start([{ name: 'a', cards: ['a'] }, { name: 'b', cards: ['b'] }], 100)
for (const bird of team.birds) {
  for (let i = 0; i < 1000; i++) bird.visit(`c${i}`, 'reading', 100.1)
  assert.equal(bird.scenes.length, 1)
  team.note(bird.name, { kind: 'toolStarted', name: 'run', arguments: '{"card":["start"]}' }, 100.12)
  assert.equal(bird.card(100.12), 'start')
  assert.equal(bird.line(100.12)?.text, 'start から実行しています')
  assert.equal(bird.place(100.12, () => ({ x: 80, y: 120 }))?.flying, false)
}

// World-space flights and their trails receive the camera transform once,
// including when the camera pans, zooms or scrolls halfway through a flight.
for (const zoom of [0.25, 0.7, 1, 2.5]) {
  const b = team.birds[0]
  const perch = (card: string) => card === 'a' ? { x: 100, y: 150 } : { x: 600, y: 350 }
  b.visit('a', 'a', 200, true)
  b.place(200, perch)
  b.visit('b', 'b', 201)
  b.place(201, perch)
  const mid = b.place(201.12, perch)!
  const screen = (x: number, y: number, z: number) => ({ x: mid.x * z + x, y: mid.y * z + y })
  const before = screen(40, 60, zoom), after = screen(113, 21, zoom)
  assert.ok(Math.abs(after.x - before.x - 73) < 1e-9)
  assert.ok(Math.abs(after.y - before.y + 39) < 1e-9)
  assert.deepEqual(b.place(201.12, perch), mid, 'camera changes never restart the flight')
}
console.log('presence: passed')

// The main bird's DOM anchor changes by exactly the Card's screen displacement,
// including off-screen positions (no clamp or side switch on a moving canvas).
for (const zoom of [0.25, 0.7, 1, 2.5]) {
  for (const x of [-1000, 100, 2000]) {
    const before = canvasBirdBox({ x, y: 150 }, zoom)
    const after = canvasBirdBox({ x: x + 73, y: 111 }, zoom)
    assert.ok(Math.abs(after.left - before.left - 73) < 1e-9)
    assert.ok(Math.abs(after.top - before.top + 39) < 1e-9)
    assert.equal(after.width, before.width)
  }
}

// The production document/run adapter starts a single cue at the landed delta
// and clears it on the actual accepted-run notification, independently of chat.
{
  const { ProjectSession } = await import('../src/model/session')
  const R = await import('../shared/gen/tsugumi/rpc')
  const D = await import('../shared/gen/tsugumi/data')
  const s = new ProjectSession('/presence', 'k', 'presence')
  s.project = s.confirmedProject = parse('{"canvas":{"cards":[],"connections":[]}}')
  const adapter = s as unknown as { applyDelta(d: InstanceType<typeof R.DocumentDeltaT>): void; applyRunEvent(e: InstanceType<typeof R.RunEventT>): void }
  const delta = Object.assign(new R.DocumentDeltaT(), { baseRev: s.rev, rev: s.rev + 1n, origin: 'agent',
    projectJson: JSON.stringify({ canvas: { cards: [], connections: [{ from: { card: 'a', port: 'o' }, to: { card: 'b', port: 'i' } }] } }) })
  adapter.applyDelta(delta)
  assert.equal(s.presence.step?.wires.size, 1)
  const status = Object.assign(new R.RunStatusT(), { runId: 'r', state: D.RunState.running })
  const event = Object.assign(new R.RunEventT(), { valueType: R.RunEventValue.RunAcceptedEvent,
    value: Object.assign(new R.RunAcceptedEventT(), { status }) })
  adapter.applyRunEvent(event)
  assert.equal(s.presence.step, undefined)
  assert.equal(s.currentRunId, 'r')
}
