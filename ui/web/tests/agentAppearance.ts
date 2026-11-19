import assert from 'node:assert/strict'
import { AgentAppearance } from '../src/model/agentAppearance'

const started = Promise.withResolvers<void>()
const reply = Promise.withResolvers<{ animateWork: boolean }>()
const requests: unknown[] = []
let saveReplied = false
const appearance = new AgentAppearance(async (body) => {
  requests.push(body)
  if (body) {
    started.resolve()
    const settings = await reply.promise
    saveReplied = true
    return settings
  }
  assert.ok(saveReplied, 'a focus refresh waits for the save reply')
  return { animateWork: false }
})
const edit = appearance.setAnimateWork(false)
await started.promise
const refresh = appearance.reload()
reply.resolve({ animateWork: false })
await Promise.all([edit, refresh])
assert.deepEqual(requests, [{ animateWork: false }, undefined])
assert.equal(appearance.animateWork, false)
assert.equal(appearance.error, undefined)

const failed = new AgentAppearance(async () => { throw new Error('conflict') })
await failed.setAnimateWork(false)
assert.equal(failed.animateWork, true, 'a rejected save leaves the current setting in place')
assert.ok(failed.error)
