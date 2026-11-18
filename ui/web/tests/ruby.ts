import { cardIds } from './cardIds'
import assert from 'node:assert/strict'
import { ProjectSession, TEMPLATES } from '../src/model/session'
import { get, parse, str } from '../shared/json'

assert(TEMPLATES.some(entry => entry.id === 'ruby'))
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
console.log('Ruby scaffold: passed')
