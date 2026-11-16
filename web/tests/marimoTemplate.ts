import { cardIds } from './cardIds'
import assert from 'node:assert/strict'
import { ProjectSession, TEMPLATES } from '../src/model/session'
import { parse, get } from '../src/shared/json'
import { builtinCardItems } from '../src/ui/canvas'
const session = cardIds(new ProjectSession('/marimo-test','marimo-test','test'))
session.project=parse('{"name":"test","canvas":{"cards":[],"connections":[]}}')
session.layout=parse('{}')
session.scheduleSave=()=>{}
const card=await session.addCard('marimo',[],{x:0,y:0})
assert.equal(card,'marimo')
assert.ok(session.sourceEdits.get(`definitions/${card}/src/notebook.py`))
const saved = session.docState()
assert.ok(TEMPLATES.some(t=>t.id==='marimo'))
assert.ok(builtinCardItems(()=>{}).some(t=>t && typeof t!=='string' && t.label.includes('marimo')))
await session.undo();assert.equal(session.cardDefinition(card),undefined)
await session.redo();assert.deepEqual(session.docState(),saved)
console.log('marimo template: creation, mapping/source, artifacts, fresh workers and undo passed')
