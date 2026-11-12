import assert from 'node:assert/strict'
import { ProjectSession, TEMPLATES } from '../src/model/session'
import { parse, get } from '../src/shared/json'
import { builtinCardItems } from '../src/ui/canvas'
import files from '../../ui/templates/marimo.json'
const session = new ProjectSession('/marimo-test','marimo-test','test')
session.project=parse('{"name":"test","canvas":{"cards":[],"connections":[]}}')
session.layout=parse('{}')
session.scheduleSave=()=>{}
const card=session.addCard('marimo',[],{x:0,y:0})
assert.equal(card,'marimo')
assert.equal(get(session.cardDefinition(card),'reuseWorker'),false)
assert.equal(get(session.cardDefinition(card),'artifactOutput'),'report')
for (const file of ['src/main.py','src/notebook.py','default.nix','_outputs.fbs'] as const) {
  assert.equal(session.sourceEdits.get(`definitions/${card}/${file}`),files[file])
}
assert.ok(TEMPLATES.some(t=>t.id==='marimo'))
assert.ok(builtinCardItems(()=>{}).some(t=>t && typeof t!=='string' && t.label.includes('marimo')))
session.undo();assert.equal(session.cardDefinition(card),undefined)
session.redo();assert.equal(get(session.cardDefinition(card),'reuseWorker'),false)
console.log('marimo template: creation, mapping/source, artifacts, fresh workers and undo passed')
