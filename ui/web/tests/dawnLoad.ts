import assert from 'node:assert/strict'
import { checkViewAbi, loadView, viewExport } from '../src/dawn/loadView'

import { encodeView, i32 } from './dawnFixture'

const { module: valid } = await loadView(encodeView())
const { exports } = new WebAssembly.Instance(valid)
checkViewAbi((exports.tsugumi_view_abi as () => number)())
for (const name of ['message', 'pointer', 'key'] as const) assert.equal(exports[viewExport(name)], undefined)
assert.equal((exports.tsugumi_view_draw as (target: number, present: bigint) => number)(1, 0n) & 1, 1)

await assert.rejects(loadView(new Uint8Array([0, 1, 2])), WebAssembly.CompileError)
await loadView(encodeView({ imports: [{ module: 'webgpu', name: 'wgpuDeviceCreateBuffer', params: [i32, i32] }] }))
await loadView(encodeView({ imports: [{ module: 'tsugumi', name: 'tsugumi_view_read', params: [i32, i32] }] }))
for (const [module, name] of [
  ['env', 'foreign'], ['webgpu', 'wgpuInvalid'], ['tsugumi', 'tsugumi_view_unknown'],
]) {
  await assert.rejects(loadView(encodeView({ imports: [{ module, name }] })), /Unsupported Dawn view import/)
}
await assert.rejects(loadView(encodeView({
  imports: [{ module: 'webgpu', name: 'wgpuDeviceCreateBuffer', memory: true }],
})), /\(memory\)/)
for (const missing of ['memory', 'abi', 'start', 'resize', 'draw'] as const) {
  await assert.rejects(loadView(encodeView({ missing })), new RegExp(`must export ${missing === 'memory' ? missing : viewExport(missing)}`))
}
const { module: wrongAbi } = await loadView(encodeView({ callbacks: { abi: { code: [0x41, 1] } } }))
const abi = new WebAssembly.Instance(wrongAbi).exports.tsugumi_view_abi as () => number
assert.throws(() => checkViewAbi(abi()), /Unsupported Dawn view ABI: 1/)

console.log('Dawn view load rules passed')
