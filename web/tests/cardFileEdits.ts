import assert from 'node:assert/strict'
import { ProjectSession } from '../src/model/session'
import { parse, serialize, get } from '../src/shared/json'
import { cardIds } from './cardIds'

const binary = new Uint8Array([0, 255, 128, 195, 40, 10])
function fixture() {
  const s = cardIds(new ProjectSession('/p', 'k', 'p'))
  const project = '{"name":"p","pkgsPath":"https://example.test/pinned#sha256-pin","nixSources":[{"name":"science","type":"flake","ref":"github:org/science/revision?narHash=sha256-pin","overrides":{"tool":{"override":{"flag":true}}}}],"canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}}'
  s.project = parse(project); s.confirmedProject = parse(project)
  for (const id of ['a', 'b']) {
    s.confirmedDefinitions.set(id, parse('{"language":"bash"}'))
    s.sources.set(`definitions/${id}/data.bin`, { hash: id, size: BigInt(binary.length) })
  }
  s.scheduleSave = () => {}
  s.client = { readFile: async () => binary.slice() } as never
  return s
}
const state = (s: ProjectSession) => ({
  project: serialize(s.project), definitions: [...s.definitionDrafts], externals: [...s.effectiveExternals],
  edits: [...s.sourceEdits], removals: [...s.sourceRemovals], undo: s.undoStack.length,
})
for (const operation of ['nest', 'duplicate'] as const) {
  const s = fixture(), before = state(s)
  s.client = { readFile: async (p: string) => {
    if (p.includes('/b/')) throw new Error('read refused')
    return binary.slice()
  } } as never
  const result = await (operation === 'nest' ? s.makeSubgraph(new Set(['a', 'b']), []) : s.duplicate(new Set(['a', 'b']), []))
  assert.deepEqual(result, operation === 'nest' ? undefined : new Set(['a', 'b']))
  assert.match(s.notice!, /definitions\/b\/data.bin.*read refused/)
  assert.deepEqual(state(s), before, `${operation}: even a later failed read changes nothing`)
}
{
  const s = fixture()
  // Moving a Card in Card includes every descendant's documents and binary files.
  s.confirmedLinks.set('b', parse('{"external":"child"}'))
  s.confirmedDefinitions.delete('b')
  for (const prefix of ['externals/child/', 'externals/child/externals/grand/']) {
    s.externals.set(prefix, { project: parse('{"name":"inner","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}}'), layout: parse('{}'), definitions: new Map() })
    s.sources.set(prefix + 'asset.bin', { hash: prefix, size: BigInt(binary.length) })
  }
  let edits = 0; s.scheduleSave = () => { edits++ }
  const k = await s.makeSubgraph(new Set(['a', 'b']), [])
  assert.equal(k, 'sub'); assert.equal(edits, 1, 'the conversion is one edit/save')
  assert.ok(s.external('externals/sub/'))
  for (const p of ['definitions/a/data.bin', 'externals/child/asset.bin', 'externals/child/externals/grand/asset.bin']) {
    assert.deepEqual(s.sourceEdits.get('externals/sub/' + p), binary, p)
  }
  assert.ok(s.external('externals/sub/externals/child/externals/grand/'))
  // The one save carries new files and original removals together.
  let saves = 0
  s.client = { readFile: async () => binary.slice(), save: async () => { saves++; return { rev: 1n } } } as never
  await (s as unknown as { saveOnce(): Promise<void> }).saveOnce()
  assert.equal(s.saveError, undefined)
  assert.equal(saves, 1)
  await s.undo()
  assert.deepEqual(s.sourceEdits.get('definitions/a/data.bin'), binary, 'undo also restores bytes')
}
{
  const s = fixture()
  const copied = await s.duplicate(new Set(['a']), [])
  assert.deepEqual([...copied], ['a-2'])
  assert.deepEqual(s.sourceEdits.get('definitions/a-2/data.bin'), binary)
  // Renaming an inner Card now changes only its metadata, keeping its files in place.
  const inner = await s.makeSubgraph(new Set(['a']), [])
  assert.equal(inner, 'sub')
  s.save = async () => {}
  // Inspect the edit without requiring a connected core to confirm the draft.
  await assert.rejects(s.renameCard('sub/a', 'renamed'), /保存/)
  assert.equal(s.displayName('sub/a'), 'renamed')
  assert.deepEqual(s.sourceEdits.get('externals/sub/definitions/a/data.bin'), binary)
}
console.log('Card file edits: atomic failed reads, binary conversion/copy/undo, nested documents passed')

// Host failures and changes made while waiting must not apply a stale edit.
{
  const s = fixture(), before = state(s)
  s.prepareSubgraph = async () => { throw new Error('host refused edit') }
  assert.equal(await s.makeSubgraph(new Set(['a', 'b']), []), undefined)
  assert.deepEqual(state(s), before)
}
{
  const s = fixture(), reply = s.prepareSubgraph
  s.prepareSubgraph = async (...args) => {
    const prepared = await reply(...args)
    s.project = parse('{"name":"changed while waiting","canvas":{"cards":[{"id":"a"},{"id":"b"}]}}')
    return prepared
  }
  assert.equal(await s.makeSubgraph(new Set(['a', 'b']), []), undefined)
  assert.equal(get(s.project, 'name'), 'changed while waiting')
  assert.equal(s.undoStack.length, 0)
}

// A copied binary must survive save -> undo -> redo before the next autosave.
{
  const { clone, str } = await import('../src/shared/json')
  const { R, s: text, sha256Hex } = await import('../src/shared/rpc')
  const s = fixture(), copied = [...await s.duplicate(new Set(['a']), [])][0]
  const target = `definitions/${copied}/data.bin`
  let saves = 0
  s.client = { readFile: async () => binary.slice(), save: async (_rev: bigint, _op: string, _origin: string, changes: R.DocumentChangeT[]) => {
    saves++
    for (const change of changes) {
      if (change.valueType === R.DocumentChangeValue.PutDefinition) {
        const value = change.value as R.PutDefinitionT
        s.confirmedDefinitions.set(text(value.id), parse(text(value.cardJson)))
        if (value.metaJson) s.confirmedDefinitionMeta.set(text(value.id), parse(text(value.metaJson)))
      }
      if (change.valueType === R.DocumentChangeValue.PutSource) {
        const value = change.value as R.PutSourceT
        const bytes = new Uint8Array(value.bytes)
        s.sources.set(text(value.path), { hash: await sha256Hex(bytes), size: BigInt(bytes.length) })
      }
    }
    s.confirmedProject = clone(s.project); s.confirmedLayout = clone(s.layout)
    return { rev: 1n }
  } } as never
  await (s as unknown as { saveOnce(): Promise<void> }).saveOnce()
  assert.equal(saves, 1)
  assert.equal(s.sourceEdits.has(target), false, 'the save has confirmed the copied bytes')
  await s.undo()
  assert.equal(s.instance(copied), undefined)
  await s.redo()
  assert.equal(str(get(s.instance(copied), 'id')), copied)
  assert.deepEqual(s.sourceEdits.get(target), binary, 'redo retains the bytes after autosave')
}
