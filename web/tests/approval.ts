import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { Agent } from '../server/agent'
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-approval-'))
try {
  const a = Object.create(Agent.prototype) as any
  a.storage = path.join(dir, 'chat.json'); a.references = {}; a.conversation = { tools: [] }
  a.catalog = { tool: () => ({ payloadType: 1, object: 1, mutating: true }), replyNames: new Map([[1, 'Failure'], [2, 'SaveResult']]) }
  let sent = '', calls = 0
  a.codec = { build: (_: unknown, args: any) => { sent = args.op_id; return 0 } }
  a.approver = async (asked: string[]) => new Set(asked)
  a.rpc = async () => ({ callRaw: async (_: unknown, build: (b: unknown) => void) => {
    build({}); calls++
    const saved = JSON.parse(fs.readFileSync(a.storage, 'utf8')).tools[0]
    assert.equal(saved.opId, sent, `operation ${calls} was not persisted before sending`)
    assert.equal(saved.state, 'sent')
    return { type: calls }
  } })
  a.renderReply = (type: string) => JSON.stringify(type === 'Failure'
    ? { reply: type, value: { kind: 'approval_required', args: [{ key: 'permission', value: 'network card' }] } }
    : { reply: type, value: {} })
  await a.runTool('tool-1', 'project_save', '{}', () => {})
  assert.equal(calls, 2)
} finally { fs.rmSync(dir, { recursive: true, force: true }) }
console.log('approval persistence: passed')
