import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { Agent, ToolCatalog } from '../server/agent'
import { BFBS } from '../src/shared/bfbs'
import { FlatJSON } from '../src/shared/flatjson'
import { Builder, ByteBuffer } from 'flatbuffers'

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-recovery-'))
try {
  const schema = new BFBS(new Uint8Array(fs.readFileSync(path.join(process.argv[2], 'web/src/gen/tsugumi.bfbs'))))
  const cases = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'tests/fixtures/agent-behavior.json'), 'utf8')).recovery
  for (const c of cases) {
    const a = Object.create(Agent.prototype) as any
    a.storage = path.join(dir, c.name + '.json')
    a.conversation = { tools: [{ id: 't', name: 'project_save', state: 'sent', opId: 'op', isError: true }],
      messages: [{ role: 'user', content: [{ type: 'tool_result', tool_use_id: 't', content: 'old', is_error: true }] }] }
    a.catalog = new ToolCatalog(schema)
    a.codec = new FlatJSON(schema)
    const completed = c.error !== null
    const builder = new Builder(256)
    const object = schema.object(c.reply === 'Failure' ? 'Tsugumi.Data.Failure' : `Tsugumi.RPC.${c.reply}`)
    builder.finish(a.codec.build(object, c.value, builder))
    const bb = new ByteBuffer(builder.asUint8Array()), table = bb.readInt32(bb.position()) + bb.position()
    const type = [...a.catalog.replyNames.entries()].find(([, name]) => name === c.reply)![0]
    a.rpc = async () => ({ callRaw: async () => ({ type, bb, table }) })
    const result = a.renderReply(c.reply, bb, table)
    const events: any[] = []
    await a.reconcilePending((e: any) => events.push(e))
    assert.equal(a.conversation.tools[0].state, completed ? 'completed' : 'sent', c.name)
    assert.equal(events.length, completed ? 1 : 0, c.name)
    if (completed) assert.equal(events[0].ok, !c.error, c.name)
    assert.equal(a.conversation.messages[0].content[0].content, result, c.name)
    assert.equal(a.conversation.messages[0].content[0].is_error, c.error ?? true, c.name)
    assert.deepEqual(JSON.parse(fs.readFileSync(a.storage, 'utf8')), a.conversation, c.name)
  }
} finally { fs.rmSync(dir, { recursive: true, force: true }) }
console.log('operation recovery: passed')
