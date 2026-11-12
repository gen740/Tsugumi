import assert from 'node:assert/strict'
import { ProjectSession, TEMPLATES } from '../src/model/session'
import { get, parse, str } from '../src/shared/json'

const s = new ProjectSession('/ruby', 'ruby', 'ruby')
s.project = parse('{"name":"ruby","canvas":{"cards":[],"connections":[]}}')
s.layout = parse('{}')
s.scheduleSave = () => {}
assert(TEMPLATES.some((entry) => entry.id === 'ruby'))
const card = s.addCard('ruby', [], { x: 0, y: 0 }, 'ruby')
assert.equal(str(get(s.cardDefinition(card), 'language')), 'ruby')
assert.match(await s.sourceText(`definitions/${card}/default.nix`), /rubyCard/)
assert.match(await s.sourceText(`definitions/${card}/src/main.rb`), /def main\(ctx\)/)
s.undo()

// Offered in the canvas's "Add Card" menu, not only known to the session.
const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { builtinCardItems } = await import('../src/ui/canvas')
const offered: string[] = []
for (const item of builtinCardItems((t) => offered.push(t))) if (item && typeof item === 'object' && item.action) item.action()
assert(offered.includes('ruby'))
assert.equal(s.definitionDrafts.has(card), false)
console.log('Ruby scaffold: passed')
