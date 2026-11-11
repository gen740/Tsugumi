import assert from 'node:assert/strict'
import { EnvironmentSource, environmentPaths, findEnvironment, storeName } from '../src/model/environments'
const calls: string[] = []
const environment = { id: 'shared', manifest: '/nix/store/manifest', derivation: '/nix/store/build.drv', system: 'aarch64-linux', pathCount: 3n }
const source: EnvironmentSource = {
  async environments(run, offset) {
    calls.push(`${run}:${offset}`)
    return offset === 0n ? { environments: [], next: 100n } : { environments: [environment], next: null }
  },
  async paths(run, id, offset, query) {
    calls.push(`${run}:${id}:${offset}`)
    assert.equal(query, 'PYTHON')
    return { paths: ['/nix/store/python'], next: 2n, total: 3n }
  },
}
assert.equal(await findEnvironment(source, 'old-run', 'shared'), environment)
assert.deepEqual(calls, ['old-run:0', 'old-run:100'])
assert.equal(await findEnvironment(source, 'old-run', 'missing'), undefined)
const before = calls.length
assert.equal(await findEnvironment(source, 'old-run', ''), undefined)
assert.equal(calls.length, before)
assert.deepEqual(await environmentPaths(source, 'imported-run', 'shared', 0n, 'PYTHON'), { paths: ['/nix/store/python'], next: 2n, total: 3n })
assert.equal(calls.length, before + 1, 'Searching reads just one page')
assert.equal(storeName('/nix/store/' + 'a'.repeat(32) + '-python-3.13'), 'python-3.13')
const abort = new AbortController()
abort.abort()
await assert.rejects(findEnvironment(source, 'other-run', 'shared', abort.signal), { name: 'AbortError' })
assert.equal(calls.length, before + 1, 'Cancelled navigation makes no request')
await assert.rejects(findEnvironment({ ...source, async environments() { throw Error('offline') } }, 'run', 'shared'), /offline/)
await assert.rejects(findEnvironment({ ...source, async environments() { return { environments: [], next: 0n } } }, 'run', 'shared'), /did not advance/)
console.log('recorded environments: passed')

const { compareCardEnvironments, groupCardEnvironments } = await import('../src/model/environments')
const oldCards = [{ card: 'same', environment: 'shared' }, { card: 'changed', environment: 'old' }, { card: 'removed', environment: 'old' }, { card: 'unknown' }]
const newCards = [{ card: 'same', environment: 'shared' }, { card: 'changed', environment: 'new' }, { card: 'added', environment: 'shared' }, { card: 'unknown' }]
assert.deepEqual(compareCardEnvironments(oldCards, newCards).map(r => [r.card, r.state]), [['added', 'added'], ['changed', 'changed'], ['removed', 'removed'], ['same', 'same'], ['unknown', 'unrecorded']])
assert.deepEqual(groupCardEnvironments(newCards), [{ id: 'shared', cards: ['added', 'same'] }, { id: 'new', cards: ['changed'] }, { id: '', cards: ['unknown'] }])
