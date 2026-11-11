import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import type { DemoData } from '../src/ui/demoPlayer'

const { framesOf, recordedTime, IDLE, DONE } = await import('../src/ui/demoPlayer')
const dataDir = path.join(process.argv[2], 'docs/site/player-data')

// Recorded runs include instantaneous steps, parallel Cards, and repeated loop invocations.
// The clock must advance between frames without overshooting or changing the recorded duration.
for (const file of fs.readdirSync(dataDir).filter((file) => file.endsWith('.json') && file !== 'index.json')) {
  const data: DemoData = JSON.parse(fs.readFileSync(path.join(dataDir, file), 'utf8'))
  if (!data.run?.realOrder) continue
  for (const speed of [1, 0.6]) {
    const frames = framesOf(data, speed)
    let previous = 0
    for (const frame of frames) {
      const start = recordedTime(frame, 0)!
      const middle = recordedTime(frame, frame.ms / 2)!
      const end = recordedTime(frame, frame.ms)!
      assert.ok(start >= previous - 1e-9, `${file}: clock must not run backward`)
      assert.ok(middle >= start && middle <= end)
      if (end > start) assert.ok(middle > start && middle < end, `${file}: clock advances within a frame`)
      assert.equal(recordedTime(frame, -10), start)
      assert.equal(recordedTime(frame, frame.ms + 100), end)
      previous = end
    }
    for (const [step, record] of data.steps.entries()) {
      const group = frames.filter((frame) => frame.step === step)
      assert.equal(recordedTime(group[0], 0), record.startMs)
      const last = group.at(-1)!
      assert.ok(Math.abs(recordedTime(last, last.ms)! - record.endMs!) < 1e-9)
    }
    assert.ok(previous <= data.run.totalMs)
  }
}
assert.equal(recordedTime(IDLE, 100), undefined)
assert.equal(recordedTime(DONE, 100), undefined)
assert.equal(recordedTime({ mode: 'playing', step: 0, sub: 0, tick: 0, ms: 500 }, 250), undefined)
console.log('demo replay clock: passed')
