import assert from 'node:assert/strict'
import { ProjectSession, TEMPLATES, definitionKind } from '../src/model/session'
import { visualizationTemplates, visualizationFiles } from '../src/model/visualizationTemplates'
import { parse, get, str } from '../src/shared/json'
import { builtinCardItems } from '../src/ui/canvas'

assert.equal(visualizationTemplates.length, 9)
const menus = builtinCardItems(() => {})
assert.ok(menus.some(item => item && typeof item !== 'string' && item.submenu?.length === 3))
for (const template of visualizationTemplates) {
  const session = new ProjectSession('/visualization-test', template.id, 'test')
  session.project = parse('{"name":"test","canvas":{"cards":[],"connections":[]}}')
  session.layout = parse('{}')
  session.scheduleSave = () => {}
  const card = session.addCard(template.id, [], { x: 0, y: 0 })
  assert.equal(card, 'visualization')
  const definition = session.cardDefinition(card)
  assert.equal(definitionKind(definition), 'code')
  assert.equal(str(get(definition, 'language')), 'python')
  assert.equal(get(definition, 'retainArtifact'), true)
  assert.equal(get(definition, 'artifactOutput'), 'figure')
  const files = visualizationFiles[template.id]
  for (const name of ['src/main.py', 'default.nix', '_outputs.fbs']) {
    const edit = session.sourceEdits.get(`definitions/${card}/${name}`)!
    assert.equal(typeof edit === 'string' ? edit : new TextDecoder().decode(edit), files[name])
  }
  assert.ok(TEMPLATES.some(t => t.id === template.id))
  session.undo()
  assert.equal(session.cardDefinition(card), undefined)
  session.redo()
  assert.equal(get(session.cardDefinition(card), 'artifactOutput'), 'figure')
}
console.log('Visualization templates: library/shape menus, source, params, retention and undo passed')
