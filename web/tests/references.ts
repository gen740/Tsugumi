import assert from 'node:assert/strict'
import path from 'node:path'
import { Agent, delegateTasks } from '../server/agent'

const root = process.argv[2]
const a = Object.create(Agent.prototype) as any
a.references = { 'demos/report': path.join(root, 'demos/report') }

// A folder in one call: a Card's files, or every file under any other.
const [card, cardError] = a.readReference('demos/report/definitions/render')
assert.equal(cardError, false)
for (const f of ['_card.json', '_outputs.fbs', 'default.nix', 'src/main.py']) assert.ok(card.includes(`--- ${f}\n`), f)
const [tree, treeError] = a.readReference('demos/report')
assert.equal(treeError, false)
assert.ok(tree.split('\n').includes('definitions/render/src/main.py'))
assert.equal(a.readReference('demos/../../core')[1], true)

// A helper sees only its instruction: a placeholder would leave it to find out everything itself.
const tasks = delegateTasks(JSON.stringify({ tasks: [{ name: 'a', cards: ['a'], instruction: '__COMMON__ Your Card: a' }, { name: 'b', cards: ['b'], instruction: 'do __init__.py' }] }))
assert.ok(typeof tasks === 'string' && tasks.includes('placeholder'))
console.log('references: passed')
