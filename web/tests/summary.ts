import { cardIds } from './cardIds'
// A Card's summary is in its _meta.json, whatever its kind, and only there.
import assert from 'node:assert/strict'
import { get, parse, str } from '../src/shared/json'

// The modules read the page when they load, and listen to the server.
const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { CardGeometry } = await import('../src/model/canvasKit')
const { ProjectSession } = await import('../src/model/session')
const { CardEditorModel } = await import('../src/ui/cardSettings')

const session = () => {
  const s = cardIds(new ProjectSession('/p', 'k', 'p'))
  s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  s.layout = parse('{}')
  s.scheduleSave = () => {} // no tsugumid here
  s.save = async () => {}
  s.applyEdit = async (_rev, change) => { change(s); return s.rev }
  s.connected = true
  return s
}

{
  const s = session()
  const code = await s.addCard('python', [], { x: 0, y: 0 })
  s.updateCard(code, (c) => c.set('description', 'in the settings'))
  assert.equal(s.nodeData(code).summary, undefined, 'the settings hold no summary')
  assert.equal(s.cardDescription(code), undefined)

  // Written in Card settings, it goes to _meta.json and shows on the Card and on hover.
  const editor = new CardEditorModel(s, code)
  editor.meta = parse('{"description":"logs the value"}')
  assert.ok(await editor.apply())
  assert.equal(str(get(s.cardMeta(code), 'description')), 'logs the value')
  assert.equal(s.nodeData(code).summary, 'logs the value')
  assert.deepEqual(s.cardDetails(code), ['logs the value'])
  assert.ok(new CardGeometry(s.nodeData(code), 'topToBottom').summary)
}

{
  // The small Cards keep their shape: the summary is on hover.
  const s = session()
  for (const kind of ['trigger', 'fork', 'merge', 'branch'] as const) {
    const k = await s.addCard(kind, [], { x: 0, y: 0 })
    s.updateMeta(k, (m) => m.set('description', 'what it does'))
    assert.ok(!new CardGeometry(s.nodeData(k), 'topToBottom').summary, kind)
    assert.deepEqual(s.cardDetails(k), ['what it does'], kind)
  }
}

{
  // A Card in Card has no _card.json; Card settings saves its summary all the same.
  const s = session()
  s.setEffectiveDefinition('nested', { card: parse('{}'), meta: undefined, link: parse('{"external":"nested"}') })
  s.project = parse('{"name":"t","canvas":{"cards":[{"id":"nested"}],"connections":[]}}')
  const editor = new CardEditorModel(s, 'nested')
  editor.meta = parse('{"description":"the imported graph"}')
  assert.ok(await editor.apply())
  assert.equal(str(get(s.cardMeta('nested'), 'description')), 'the imported graph')
  assert.equal(s.cardDescription('nested'), 'the imported graph')
  assert.ok(s.cardDefinitionDraft('nested')?.link !== undefined, 'it stays a link')
}
console.log('summary behavior: passed')
