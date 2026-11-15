import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { INITIAL_INSTRUCTIONS, INSTRUCTIONS } from '../server/mcpInstructions'
const shared = path.join(process.argv[2], 'agent/Shared')
const rules = fs.readFileSync(path.join(shared, 'system-prompt.md'), 'utf8').split('- Making a Draft (')[0]
assert.equal(INITIAL_INSTRUCTIONS, fs.readFileSync(path.join(shared, 'mcp-transport.md'), 'utf8'))
assert.ok(INITIAL_INSTRUCTIONS.includes('tsugumi_open') && INITIAL_INSTRUCTIONS.length <= 4096)
assert.equal(INSTRUCTIONS, INITIAL_INSTRUCTIONS + '\n' + rules)
console.log('Plugin instructions contain the shared Agent save/run/approval rules: passed')
