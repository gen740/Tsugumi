import assert from 'node:assert/strict'
import { ProjectSession } from '../src/model/session'

const originalFetch = globalThis.fetch
const decisions = new Map<string, number>()
const requests: { path: string; decision?: number }[] = []
globalThis.fetch = (async (url, init) => {
  assert.equal(String(url), '/api/project/trust')
  const request = JSON.parse(String(init?.body))
  requests.push(request)
  if (request.decision) decisions.set(request.path, request.decision)
  return new Response(JSON.stringify({ decision: request.path === '/trials/project' ? 1 : decisions.get(request.path) ?? 0, trusted_folder: request.path === '/trials/project' ? '/trials' : '' }))
}) as typeof fetch
try {
  const session = new ProjectSession('/trust-project', 'trust-key', 'Project')
  await session.refreshProjectTrust()
  assert.equal(session.projectTrust, 0)
  await session.setProjectTrust(false)
  assert.equal(session.projectTrust, 2)
  const reopened = new ProjectSession('/trust-project', 'trust-key', 'Project')
  await reopened.refreshProjectTrust()
  assert.equal(reopened.projectTrust, 2)
  await reopened.setProjectTrust(true)
  assert.equal(reopened.projectTrust, 1)
  assert.deepEqual(requests.map(x => x.decision), [undefined, 2, undefined, 1])
  const inherited = new ProjectSession('/trials/project', 'folder-key', 'Project')
  await inherited.refreshProjectTrust()
  assert.equal(inherited.projectTrust, 1)
  assert.equal(inherited.trustedFolder, '/trials')
  inherited.dispose()
  session.dispose()
  reopened.dispose()
} finally { globalThis.fetch = originalFetch }
console.log('project trust: passed')
