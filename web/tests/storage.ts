import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { Agent, loadConversation, newConversation } from '../server/agent'
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-storage-'))
try {
  const file = path.join(dir, 'chat.json')
  assert.deepEqual(loadConversation(file, 'id').messages, [])
  for (const broken of ['{broken history', '{}', '{"messages":null}']) {
    fs.writeFileSync(file, broken)
    assert.throws(() => loadConversation(file, 'id'))
    assert.equal(fs.readFileSync(file, 'utf8'), broken)
  }
  assert.throws(() => loadConversation(dir, 'id'))
  const conversation = newConversation('id')
  fs.writeFileSync(file, JSON.stringify(conversation))
  assert.deepEqual(loadConversation(file, 'id'), conversation)
  for (const approvedRetry of [false, true]) {
    const a = Object.create(Agent.prototype) as any
    a.storage = path.join(dir, `fail-${approvedRetry}.json`); a.references = {}; a.conversation = newConversation('id')
    a.catalog = { tool: () => ({ payloadType: 1, object: 1, mutating: true }), replyNames: new Map([[1, 'Failure']]) }
    a.codec = { build: () => 0 }
    let calls = 0
    a.rpc = async () => ({ callRaw: async () => { calls++; return { type: 1 } } })
    a.renderReply = () => JSON.stringify({ reply: 'Failure', value: { kind: 'approval_required', args: [{ key: 'permission', value: 'network card' }] } })
    const blockSave = () => { fs.rmSync(a.storage, { force: true }); fs.mkdirSync(a.storage) }
    if (approvedRetry) a.approver = async (asked: string[]) => { blockSave(); return new Set(asked) }
    else blockSave()
    await assert.rejects(() => a.runTool('tool', 'project_save', '{}', () => {}))
    assert.equal(calls, approvedRetry ? 1 : 0)
  }
} finally { fs.rmSync(dir, { recursive: true, force: true }) }
console.log('conversation storage: passed')
