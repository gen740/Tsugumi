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
  for (const state of ['pending', 'unknown', 'success', 'failure']) {
    const a = Object.create(Agent.prototype) as any
    a.storage = path.join(dir, state + '.json')
    a.conversation = { tools: [{ id: 't', name: 'project_save', state: 'sent', opId: 'op', isError: true }],
      messages: [{ role: 'user', content: [{ type: 'tool_result', tool_use_id: 't', content: 'old', is_error: true }] }] }
    a.catalog = new ToolCatalog(schema)
    a.codec = new FlatJSON(schema)
    const completed = ['success', 'failure'].includes(state)
    const value = { state: completed ? 'completed' : state,
      ...(completed ? { receipt: { op_id: 'op', result_type: state === 'failure' ? 'Failure' : 'SaveResult',
        result: state === 'failure' ? { kind: 'validation_failed', args: [] } : {} } } : {}) }
    const builder = new Builder(256)
    builder.finish(a.codec.build(schema.object('Tsugumi.RPC.OperationResult'), value, builder))
    const bb = new ByteBuffer(builder.asUint8Array()), table = bb.readInt32(bb.position()) + bb.position()
    const type = [...a.catalog.replyNames.entries()].find(([, name]) => name === 'OperationResult')![0]
    a.rpc = async () => ({ callRaw: async () => ({ type, bb, table }) })
    const result = a.renderReply('OperationResult', bb, table)
    const events: any[] = []
    await a.reconcilePending((e: any) => events.push(e))
    assert.equal(a.conversation.tools[0].state, completed ? 'completed' : 'sent', state)
    assert.equal(events.length, completed ? 1 : 0)
    if (completed) assert.equal(events[0].ok, state === 'success')
    assert.equal(a.conversation.messages[0].content[0].content, result)
    assert.equal(a.conversation.messages[0].content[0].is_error, state !== 'success')
  }
} finally { fs.rmSync(dir, { recursive: true, force: true }) }
console.log('operation recovery: passed')
