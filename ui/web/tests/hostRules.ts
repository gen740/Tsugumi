import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { INITIAL_INSTRUCTIONS, INSTRUCTIONS } from '../server/mcpInstructions'
const shared = path.join(process.argv[2], 'hostd/agent/Shared')
const rules = fs.readFileSync(path.join(shared, 'system-prompt.md'), 'utf8')
assert.equal(INITIAL_INSTRUCTIONS, fs.readFileSync(path.join(shared, 'mcp-transport.md'), 'utf8'))
assert.ok(INITIAL_INSTRUCTIONS.length <= 4096)
assert.ok(INSTRUCTIONS.includes(INITIAL_INSTRUCTIONS.trim()) && INSTRUCTIONS.includes(rules.split("\n").find(line => line.startsWith("- project_inspect"))!))
console.log('Plugin instructions contain the shared Agent save/run/approval rules: passed')
