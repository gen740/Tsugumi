import assert from 'node:assert/strict'
import { AgentAppearance } from '../src/model/agentAppearance'

const calls: unknown[] = []
let stored = true
const setting = new AgentAppearance(async (body) => {
  calls.push(body)
  if (body) stored = body.animateWork
  return { animateWork: stored }
})
assert.equal(setting.animateWork, true)
await setting.setAnimateWork(false)
assert.deepEqual(calls, [{ animateWork: false }])
assert.equal(setting.animateWork, false)
const other = new AgentAppearance(async () => ({ animateWork: stored }))
await other.reload()
assert.equal(other.animateWork, false, 'another UI reads the host value')
const failed = new AgentAppearance(async () => { throw new Error('conflict') })
await failed.setAnimateWork(false)
assert.equal(failed.animateWork, true, 'failed save does not pretend to persist')
assert.equal(failed.error, 'conflict')
console.log('Agent appearance: passed')

// A focus refresh waits for an in-flight edit, then reads the stored value.
let finish: (() => void) | undefined
let value = true
const delayed = new AgentAppearance(async (body) => {
  if (body) {
    await new Promise<void>((done) => { finish = done })
    value = body.animateWork
  }
  return { animateWork: value }
})
const edit = delayed.setAnimateWork(false)
await Promise.resolve()
const reload = delayed.reload()
assert.ok(finish)
finish()
await Promise.all([edit, reload])
assert.equal(delayed.animateWork, false)
