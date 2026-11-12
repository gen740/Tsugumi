import assert from 'node:assert/strict'
import { ProjectSession, templateIcon } from '../src/model/session'
import { get, parse } from '../src/shared/json'
import files from '../../ui/templates/csharp.json'

const session = new ProjectSession('/csharp-test', 'csharp-test', 'csharp-test')
session.project = parse('{"name":"csharp-test","canvas":{"cards":[],"connections":[]}}')
session.layout = parse('{}')
session.scheduleSave = () => {}
const card = session.addCard('csharp', [], { x: 0, y: 0 })
assert.equal(get(session.cardDefinition(card), 'language'), 'csharp')
assert.equal(session.kind(card), 'code')
assert.ok(templateIcon('csharp'))
for (const [name, text] of Object.entries(files)) if (name !== '_card.json') {
  assert.equal(await session.sourceText(`definitions/${card}/${name}`), text)
}
session.undo()
assert.equal(session.instance(card), undefined)
assert.equal(session.sourcePaths.some(p => p.endsWith('Card.cs')), false)
console.log('C# Card creation and undo: passed')
