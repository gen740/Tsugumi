import assert from 'node:assert/strict'
import { parse } from '../shared/json'
import { nixPathLockStatus } from '../src/model/nixPath'

const url = 'github:owner/repo'
const lock = parse('{"nixPath":[{"name":"science","url":"github:owner/repo","locked":{"type":"github","rev":"abcdef1234","narHash":"sha256-example","lastModified":1700000000}}]}')
assert.deepEqual(nixPathLockStatus(undefined, 'science', url), { kind: 'unlocked' })
assert.deepEqual(nixPathLockStatus(lock, 'other', url), { kind: 'unlocked' })
assert.deepEqual(nixPathLockStatus(lock, 'science', url + '/other'), { kind: 'urlChanged' })
assert.deepEqual(nixPathLockStatus(lock, 'science', url), {
  kind: 'locked', revision: 'abcdef1', modified: new Date(1700000000 * 1000),
})
