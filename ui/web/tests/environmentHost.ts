import assert from 'node:assert/strict'
import * as H from '../shared/gen/tsugumi/host'
import { environmentProgress, environmentState } from '../src/wire/environment'

const ready = Object.assign(new H.EnvironmentStateT(), {
  phase: H.EnvironmentPhase.Ready, endpoint: 'ws://localhost:1/rpc', token: 'session-token',
  projectId: 'project', readyFile: '/ready', executionScopeUnavailable: true,
})
assert.deepEqual(environmentState(ready), {
  state: 'ready', endpoint: 'ws://localhost:1/rpc', token: 'session-token', projectId: 'project',
  readyFile: '/ready', executionScopeUnavailable: true,
})
const diagnostic = { kind: 'incompatible_history', found: '2', expected: '3' }
const failed = Object.assign(new H.EnvironmentStateT(), {
  phase: H.EnvironmentPhase.Failed, recovery: 'Retry the virtual machine', message: 'History open failed',
  startupError: Object.assign(new H.StartupErrorT(), diagnostic),
})
assert.equal(environmentState(failed).recovery, failed.recovery)
assert.deepEqual(environmentState(failed).historyVersionMismatch, diagnostic)
const progress = environmentProgress(new H.EnvironmentProgressT('/project', 'key', 'booting the VM', 0.5, true))
assert.equal(progress.state, 'starting')
assert.equal(progress.fraction, 0.5)
