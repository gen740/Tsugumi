import { projectFixture } from './documentFixture'
import assert from 'node:assert/strict'
import { connectionKey, SavePresence, SAVE_CUE_SECONDS } from '../src/model/presence'
import { parse } from '../shared/json'

const wire = parse('{"from":{"card":"a","port":"out"},"to":{"card":"b","port":"in"}}')
const presence = new SavePresence()
presence.saved(['a', 'b'], [], [wire], 100)
assert.deepEqual(presence.step?.cards, new Set(['a', 'b']))
assert.deepEqual(presence.step?.wires, new Set([connectionKey(wire)]))
assert.equal(presence.progress(100), 0)
assert.equal(presence.progress(100 + 2 * SAVE_CUE_SECONDS), undefined)

presence.saved(['c'], [wire], [wire], 101)
assert.deepEqual(presence.step?.cards, new Set(['c']), 'a newer save replaces the cue')
assert.equal(presence.step?.wires.size, 0, 'source edits do not reconnect existing wires')

// The production document/run adapter starts a single cue at the landed delta
// and clears it on the actual accepted-run notification, independently of chat.
{
  const { ProjectSession } = await import('../src/model/session')
  const R = await import('../shared/gen/tsugumi/rpc')
  const D = await import('../shared/gen/tsugumi/data')
  const s = new ProjectSession('/presence', 'k', 'presence')
  s.project = s.confirmedProject = parse(projectFixture({"canvas":{"cards":[],"connections":[]},"name":"t"}))
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
