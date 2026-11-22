import assert from 'node:assert/strict'
import * as H from '../shared/gen/tsugumi/host'
import { ProjectSession } from '../src/model/session'
import { newCardItems } from '../src/ui/cardTemplates'
import { parse, serialize } from '../shared/json'
import { operationFixture } from './operationFixture'

const session = operationFixture(new ProjectSession('/templates', 'templates', 'test'), {
  'templates/example/start/_card.json': '{"type":"trigger"}',
  'templates/example/plan/_card.json': '{"type":"draft"}',
  'templates/example/plan/notes.txt': 'a source file',
  'templates/example/tsugumi.json': '{"canvas":{"cards":[],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"plan","port":""}}]}}',
  'templates/example/layout.bin': '{"cards":{"start":{"x":0,"y":0},"plan":{"x":250,"y":30}}}',
})
session.project = parse('{"canvas":{"cards":[],"connections":[]}}')
session.layout = parse('{}')
const entry = new H.TemplateEntryT(H.TemplateStore.Project, 'example')
const at = { x: 10, y: 20 }
await session.copyTemplate(entry, [], at)
assert.equal(session.kind('start'), 'trigger')
assert.equal(session.kind('plan'), 'draft')
assert.deepEqual(session.position('start'), at)
assert.deepEqual(session.position('plan'), { x: 260, y: 50 })
assert.ok(session.sources.has('definitions/plan/notes.txt'))
assert.equal(session.connections([]).length, 1)
const saved = serialize(session.project)
await session.undo()
assert.equal(session.cardDefinition('plan'), undefined)
assert.equal(session.connections([]).length, 0)
await session.redo()
assert.equal(serialize(session.project), saved)
assert.ok(session.sources.has('templates/example/plan/notes.txt'))

let picked = false
const builtins: string[] = []
for (const item of newCardItems(kind => builtins.push(kind), () => { picked = true })) {
  if (typeof item !== 'string' && item) item.action?.()
}
assert.deepEqual(builtins, ['draft', 'trigger', 'input', 'input_bool', 'branch', 'fork', 'merge', 'collect'])
assert.ok(picked)
