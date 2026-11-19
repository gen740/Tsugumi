import assert from 'node:assert/strict'
import { ProjectSession } from '../src/model/session'

const session = new ProjectSession('/trials/project', 'trust-key', 'Project')
const originalFetch = globalThis.fetch
const requests: unknown[] = []
const replies = [
  { decision: 0 },
  { decision: 1, trusted_folder: '/trials' },
  { decision: 2 },
  { decision: 1 },
]
globalThis.fetch = async (url, init) => {
  assert.equal(String(url), '/api/project/trust')
  requests.push(JSON.parse(String(init?.body)))
  return Response.json(replies.shift())
}
try {
  await session.refreshProjectTrust()
  assert.equal(session.projectTrust, 0)
  await session.refreshProjectTrust()
  assert.equal(session.projectTrust, 1)
  assert.equal(session.trustedFolder, '/trials')
  await session.setProjectTrust(false)
  assert.equal(session.projectTrust, 2)
  assert.equal(session.trustedFolder, '', 'the UI clears the previous inherited decision')
  await session.setProjectTrust(true)
  assert.equal(session.projectTrust, 1)
  assert.deepEqual(requests, [
    { path: session.dir },
    { path: session.dir },
    { path: session.dir, decision: 2 },
    { path: session.dir, decision: 1 },
  ])
} finally {
  session.dispose()
  globalThis.fetch = originalFetch
}
