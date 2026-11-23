import assert from 'node:assert/strict'
import { parse } from '../shared/json'
import { nixPathLockStatus } from '../src/model/nixPath'

const url = 'github:owner/repo'
const lock = parse('{"nixPath":{"science":"github:owner/repo/abcdef1234567890abcdef1234567890abcdef12?narHash=sha256-example&lastModified=1700000000"}}')
assert.deepEqual(nixPathLockStatus(undefined, 'science', url), { kind: 'unlocked' })
assert.deepEqual(nixPathLockStatus(lock, 'other', url), { kind: 'unlocked' })
assert.deepEqual(nixPathLockStatus(lock, 'science', 'github:owner/other'), { kind: 'urlChanged' })
for (const intent of [url, url + '/another-ref'])
  assert.deepEqual(nixPathLockStatus(lock, 'science', intent), {
    kind: 'locked', revision: 'abcdef1', modified: new Date(1700000000 * 1000),
  })
