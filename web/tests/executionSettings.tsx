import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { parse } from '../src/shared/json'
import { R } from '../src/shared/rpc'

const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')
const { CardSettingsEditor } = await import('../src/ui/cardSettings')

const session = new ProjectSession('/p', 'k', 'p')
session.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
session.layout = parse('{}')
session.scheduleSave = () => {}
session.connected = true
const card = session.addCard('python', [], { x: 0, y: 0 })
const show = (nativeSystem: string, systems: string[]) => {
  session.executionServices = [Object.assign(new R.ExecutionServiceT(), { id: 'local', nativeSystem, systems })]
  return renderToStaticMarkup(<CardSettingsEditor session={session} card={card} onClose={() => {}} />)
}

const linux = show('aarch64-linux', ['aarch64-linux', 'x86_64-linux'])
assert.match(linux, /Linux \(aarch64-linux\)/)
assert.match(linux, /この実行環境の既定（aarch64-linux）/)
assert.match(linux, /<option value="x86_64-linux">/)
assert.doesNotMatch(linux, /macOS|Rosetta/)

session.updateCard(card, (def) => def.set('system', 'x86_64-linux'))
const unavailable = show('aarch64-linux', ['aarch64-linux'])
assert.match(unavailable, /<option value="x86_64-linux" disabled="" selected="">/)
assert.match(unavailable, /この実行環境では x86_64-linux を実行できません。/)

session.updateCard(card, (def) => def.set('system', undefined))
const darwin = show('aarch64-darwin', ['aarch64-darwin'])
assert.match(darwin, /macOS \(aarch64-darwin\)/)
assert.match(darwin, /この実行環境の既定（aarch64-darwin）/)
assert.doesNotMatch(darwin, /<option value="x86_64-linux"/)

console.log('Card execution settings: passed')
