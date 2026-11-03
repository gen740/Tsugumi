// The web Agent's half of the Swift/web contract (tests/fixtures/): the tool
// catalog equals the snapshot both Agents are checked against, and FlatJSON
// and the Draft request give the shared cases' results. Run by
// scripts/contract.mjs; `--update` writes the tool snapshot instead.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { Builder, ByteBuffer } from 'flatbuffers'
import { BFBS } from '../src/shared/bfbs'
import { FlatJSON, InvalidArguments } from '../src/shared/flatjson'
import { draftRequest } from '../src/shared/agentShared'
import { approvalRequired, ToolCatalog } from './agent'

type Json = null | boolean | number | string | Json[] | { [k: string]: Json }
interface Fixture {
  flatjson: { type: string; json: { [k: string]: Json }; expect?: { [k: string]: Json }; error?: boolean }[]
  draftRequest: { scope: string[]; cards: { key: string; definition: string; description: string; content: string }[]; expect: string }[]
  approvalRequired: { json: string; expect: string[] | null }[]
}

const [root, mode] = process.argv.slice(2)
const schema = new BFBS(new Uint8Array(fs.readFileSync(path.join(root, 'web/src/gen/tsugumi.bfbs'))))

const tools = new ToolCatalog(schema).tools.map((t) => ({
  name: t.name, payloadType: t.payloadType, description: t.description, inputSchema: t.inputSchema, mutating: t.mutating,
}))
const snapshot = path.join(root, 'tests/fixtures/agent-tools.json')
if (mode === '--update') {
  fs.writeFileSync(snapshot, JSON.stringify(tools, null, 1) + '\n')
  console.log(`wrote ${snapshot}`)
} else {
  assert.deepEqual(tools, JSON.parse(fs.readFileSync(snapshot, 'utf8')),
    'the tool catalog differs from tests/fixtures/agent-tools.json (run `npm run contract -- --update` after an RPC change)')
}

const fixture = JSON.parse(fs.readFileSync(path.join(root, 'tests/fixtures/agent-contract.json'), 'utf8')) as Fixture
const codec = new FlatJSON(schema)
for (const c of fixture.flatjson) {
  const object = schema.object(c.type)
  const b = new Builder(256)
  const label = `${c.type} ${JSON.stringify(c.json)}`
  if (c.error) {
    assert.throws(() => codec.build(object, c.json, b), InvalidArguments, label)
    continue
  }
  b.finish(codec.build(object, c.json, b))
  const bb = new ByteBuffer(b.asUint8Array())
  const out = codec.json(object, bb, bb.readInt32(bb.position()) + bb.position())
  for (const [k, v] of Object.entries(c.expect ?? {})) assert.deepEqual(out[k], v, `${label}: ${k} is ${JSON.stringify(out[k])}`)
}
for (const c of fixture.draftRequest) assert.equal(draftRequest(c.scope, c.cards), c.expect)
for (const c of fixture.approvalRequired) assert.deepEqual(approvalRequired(c.json) ?? null, c.expect, c.json)
console.log(`contract: ${tools.length} tools, ${fixture.flatjson.length} FlatJSON cases, ${fixture.draftRequest.length} Draft requests`)
