import assert from 'node:assert/strict'
import { ProjectSession } from '../src/model/session'
import * as H from '../shared/gen/tsugumi/host'

const session = new ProjectSession('/trials/project', 'trust-key', 'Project')
const requests: unknown[] = []
const replies = [
  new H.ProjectTrustT(H.ProjectTrustDecision.Unknown),
  new H.ProjectTrustT(H.ProjectTrustDecision.Trusted, '/trials'),
  new H.ProjectTrustT(H.ProjectTrustDecision.Untrusted),
  new H.ProjectTrustT(H.ProjectTrustDecision.Trusted),
]
session.projectHost = { ...session.projectHost, trust: async (path, decision) => {
  requests.push(decision === undefined ? { path } : { path, decision })
  return replies.shift()!
} }
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
}
