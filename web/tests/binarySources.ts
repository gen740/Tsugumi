import assert from 'node:assert/strict'
import { cardIds } from './cardIds'
import { parse } from '../src/shared/json'
const page = globalThis as any
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.EventSource ??= class { addEventListener() {} close() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
const { ProjectSession } = await import('../src/model/session')
const bytes = Uint8Array.of(0xff, 0, 0x80)
function session() {
  const s = cardIds(new ProjectSession('/p', 'k', 'p'))
  s.project = s.confirmedProject = parse('{"canvas":{"cards":[{"id":"a"}],"connections":[]}}')
  s.layout = s.confirmedLayout = parse('{}')
  s.definitionEdits.set('a', { card: parse('{"kind":"code"}') })
  s.scheduleSave = () => {}
  s.client = { readFile: async () => bytes } as any
  return s
}
for (const draft of [false, true]) {
  const s = session(), p = 'definitions/a/src/blob.bin'
  if (draft) s.sourceEdits.set(p, bytes)
  else s.sources.set(p, { hash: 'hash', size: 3n })
  const copy = await s.copyDefinition('', 'a', [])
  assert.ok(copy)
  copy.apply(s)
  assert.deepEqual(s.sourceEdits.get(`definitions/${copy.id}/src/blob.bin`), bytes, `definition copy (draft=${draft})`)
  const external = 'externals/sub/definitions/a/src/blob.bin'
  if (draft) s.sourceEdits.set(external, bytes)
  else s.sources.set(external, { hash: 'hash', size: 3n })
  const c = await s.copyExternal('externals/sub/', 'externals/copy/')
  c.apply(s)
  assert.deepEqual(s.sourceEdits.get('externals/copy/definitions/a/src/blob.bin'), bytes, `external copy (draft=${draft})`)
}
{
  const s = session(), p = 'definitions/a/src/blob.bin'
  s.sources.set(p, { hash: 'hash', size: 3n })
  s.edit((x) => x.removeSource(p))
  await (s as any).fillUndoSources()
  s.undo()
  assert.deepEqual(s.sourceEdits.get(p), bytes, 'undo restores bytes')
  let sent: any[] = []
  s.client = { save: async (_rev: unknown, _op: unknown, _origin: unknown, changes: any[]) => { sent = changes; throw Error('captured') } } as any
  await (s as any).saveOnce().catch(() => {})
  assert.deepEqual(sent.map((c) => c.value).find((v) => v?.path === p)?.bytes, [...bytes], 'RPC writes the original bytes')
}
console.log('binary definition/external copies and undo: passed')
