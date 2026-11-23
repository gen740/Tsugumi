import assert from 'node:assert/strict'
import { Builder, ByteBuffer } from 'flatbuffers'
import { D, R, RequestPayload, RPCClient } from '../src/wire/rpc'
import { sdkBuild } from '../src/ui/cardSettings'

const file = (path: string, bytes: number[]) => new R.NixOutputFileT(path, bytes)
const result = new R.EvaluateNixResultT(42n, null, [
  file('share/tsugumi/api/README.md', [65, 80, 73]),
  file('share/tsugumi/api-extra/private', [255]),
  file('bin/tool', [0]),
], true)

const calls: R.EvaluateNixRequestT[] = []
const rpc = new RPCClient({
  async call(frame) {
    const message = R.Message.getRootAsMessage(new ByteBuffer(frame)).unpack()
    const request = message.payload as R.RequestT
    assert.equal(request.valueType, RequestPayload.EvaluateNixRequest)
    calls.push(request.value as R.EvaluateNixRequestT)
    const builder = new Builder()
    const reply = new R.MessageT(message.requestId, R.Payload.Reply,
      new R.ReplyT(R.ReplyPayload.EvaluateNixResult, result))
    builder.finish(reply.pack(builder), 'TSG1')
    return builder.asUint8Array()
  },
  close() {},
})

await rpc.evaluateNix(['plot'], 'builtins.currentSystem')
const sdk = sdkBuild(await rpc.evaluateNix(['nested', 'plot'], 'card.api', {
  build: true, system: 'aarch64-darwin', atRev: 42n,
}))
assert.deepEqual(calls.map(r => [r.card?.segments, r.expression, r.build, r.system, r.atRev]), [
  [['plot'], 'builtins.currentSystem', false, null, null],
  [['nested', 'plot'], 'card.api', true, 'aarch64-darwin', 42n],
])
assert.deepEqual(sdk, {
  building: false, files: [{ id: 'sdk:README.md', name: 'README.md', text: 'API' }], truncated: true,
})
result.failure = Object.assign(new D.FailureT(), { kind: D.FailureKind.nix_failed, diagnostic: 'failed' })
result.log = 'last line'
assert.deepEqual(sdkBuild(result), { building: false, failure: 'failed', log: 'last line' })
