// Packages in the run history: read from what an invocation recorded, never
// from the current definition, and plain about an invocation without a record.
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { EnvironmentSource, environmentPaths, findEnvironment, storeName } from '../src/model/environments'
import type { ProjectSession } from '../src/model/session'
import { R } from '../src/shared/rpc'
import { InvocationPackages, recordedEnvironmentSource } from '../src/ui/environments'

const env = (id: string) => ({ id, derivation: `${id}.drv`, manifest: id, system: 'aarch64-linux', pathCount: 3n })
const calls: string[] = []
const source: EnvironmentSource = {
  async environments(run, offset) {
    calls.push(`environments ${run} ${offset}`)
    return offset === 0n ? { environments: [env('a'), env('b')], next: 2n } : { environments: [env('c')], next: null }
  },
  async paths(run, environment, offset, query) {
    calls.push(`paths ${run} ${environment} ${offset} ${JSON.stringify(query)}`)
    return { paths: ['/nix/store/' + 'a'.repeat(32) + '-python-3.13'], next: null, total: 1n }
  },
}

// An invocation without a recorded environment: nothing is asked of the core,
// and nothing is worked out from the definition as it is now.
assert.equal(await findEnvironment(source, 'run', ''), undefined)
assert.deepEqual(calls, [])
let touched = 0
const untouchable = new Proxy({}, { get() { touched++; return undefined } }) as unknown as ProjectSession
const empty = renderToStaticMarkup(<InvocationPackages session={untouchable} runId="run" environmentId="" />)
assert.ok(empty.includes('この呼び出しの環境は記録されていません'))
assert.ok(empty.includes('現在の Nix 定義からは求めません'))
assert.equal(touched, 0, 'the session (its RPC, its document) is not consulted at all')

// A recorded environment is found by walking the run's own pages, and stops there.
assert.equal((await findEnvironment(source, 'run', 'c'))?.id, 'c')
assert.deepEqual(calls.splice(0), ['environments run 0', 'environments run 2'])
assert.equal((await findEnvironment(source, 'run', 'a'))?.id, 'a')
assert.deepEqual(calls.splice(0), ['environments run 0'])
// A Card that never ran in this run has no environment among the recorded ones.
assert.equal(await findEnvironment(source, 'run', 'never-ran'), undefined)
calls.length = 0
const aborted = new AbortController()
aborted.abort()
await assert.rejects(findEnvironment(source, 'run', 'a', aborted.signal))
assert.deepEqual(calls, [], 'a selection that changed asks for nothing')

// The search is passed to the history database with the page's offset.
assert.equal((await environmentPaths(source, 'run', 'a', 100n, 'Python')).total, 1n)
assert.deepEqual(calls.splice(0), ['paths run a 100 "Python"'])
const stuck: EnvironmentSource = { ...source, async paths() { return { paths: [], next: 5n, total: 9n } } }
await assert.rejects(environmentPaths(stuck, 'run', 'a', 5n, ''), /did not advance/)
const loop: EnvironmentSource = { ...source, async environments() { return { environments: [], next: 0n } } }
await assert.rejects(findEnvironment(loop, 'run', 'a'), /did not advance/)

// Only the two history requests are ever sent: no inspection, build or evaluation of the project.
const sent: [number, Record<string, unknown>][] = []
const rpc = {
  async call(type: number, request: Record<string, unknown>) {
    sent.push([type, { ...request }])
    return type === R.RequestPayload.RunEnvironmentsRequest
      ? { environments: [{ id: 'a', manifest: 'm', derivation: 'd', system: 'x86_64-linux', pathCount: 1n }], nextOffset: null }
      : { paths: ['/nix/store/x'], nextOffset: null, total: 1n }
  },
}
const recorded = recordedEnvironmentSource(rpc as never)
assert.equal((await findEnvironment(recorded, 'run', 'a'))?.system, 'x86_64-linux')
assert.deepEqual((await environmentPaths(recorded, 'run', 'a', 0n, 'x')).paths, ['/nix/store/x'])
assert.deepEqual(sent.map(([type]) => type), [R.RequestPayload.RunEnvironmentsRequest, R.RequestPayload.RunEnvironmentPathsRequest])
assert.deepEqual([sent[1][1].runId, sent[1][1].environmentId, sent[1][1].query, sent[1][1].offset], ['run', 'a', 'x', 0n])

assert.equal(storeName('/nix/store/' + 'a'.repeat(32) + '-python-3.13'), 'python-3.13')
assert.equal(storeName('/nix/store/short-name'), 'short-name')
console.log('recorded packages: passed')
