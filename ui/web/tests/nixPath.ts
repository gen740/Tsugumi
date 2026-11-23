import assert from 'node:assert/strict'
import { parse } from '../shared/json'
import { nixPathLockStatus } from '../src/model/nixPath'

const url = 'github:owner/repo'
const lock = parse('{"entries":[{"name":"science","url":"github:owner/repo","rev":"abcdef1234567890abcdef1234567890abcdef12","narHash":"sha256-example"}]}')
assert.deepEqual(nixPathLockStatus(undefined, 'science', url), { kind: 'unlocked' })
assert.deepEqual(nixPathLockStatus(lock, 'other', url), { kind: 'unlocked' })
assert.deepEqual(nixPathLockStatus(lock, 'science', 'github:owner/other'), { kind: 'urlChanged' })
assert.deepEqual(nixPathLockStatus(lock, 'science', url + '/another-ref'), { kind: 'urlChanged' })
assert.deepEqual(nixPathLockStatus(lock, 'science', url), { kind: 'locked', pin: 'abcdef1' })
const tarball = parse('{"entries":[{"name":"archive","url":"https://example.com/a.tgz","narHash":"sha256-example"}]}')
assert.deepEqual(nixPathLockStatus(tarball, 'archive', 'https://example.com/a.tgz'), {
  kind: 'locked', pin: 'sha256-example',
})
