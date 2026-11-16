// Force acknowledgements during a second external edit and a layout proposal.
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { ProjectSession } from '../src/model/session'
import { R } from '../src/shared/rpc'
import { clone, get, JObj, obj, parse, serialize } from '../src/shared/json'
import { hostDocumentReply } from './hostDocument'

function deferred() {
  let resolve!: () => void
  const promise = new Promise<void>(r => { resolve = r })
  return { promise, resolve }
}
function fixture() {
  const s = new ProjectSession('/save', 'save', 'save')
  s.connected = true; s.projectId = 'save'; s.scheduleSave = () => {}
  s.project = parse('{"name":"base","canvas":{"cards":[{"id":"a"}],"connections":[]}}')
  s.confirmedProject = clone(s.project)
  s.layout = parse('{"cards":{"a":{"x":0,"y":0}},"groups":[{"id":"g","scope":"","cards":["a"],"name":"group","color":"blue"}]}'); s.confirmedLayout = clone(s.layout)
  s.confirmedDefinitions.set('a', parse('{"template":"draft"}'))
  s.prepareMerge = async (baseJson, oursJson, theirsJson) => hostDocumentReply({ baseJson, oursJson, theirsJson })
  s.layoutHost = { validate: async (json: string) => json } as never
  let revision = 0n
  // Keep source number spelling, just as the core's document strings do.
  let documents = obj([['tsugumi.json', clone(s.project)], ['layout.json', clone(s.layout)], ['definitions/a/_card.json', parse('{"template":"draft"}')]])
  const sources = new Map<string, Uint8Array>()
  let beforeReply = async () => {}, beforeRead = async () => {}
  s.client = {
    readFile: async (path: string) => { await beforeRead(); return sources.get(path)! },
    save: async (_rev: bigint, opId: string, origin: string, changes: R.DocumentChangeT[], _check: boolean, _permissions: string[], mergeBase: { json: string }) => {
      const base = get(parse(mergeBase.json), 'files') as JObj, ours = clone(base)
      for (const c of changes) {
        if (c.valueType === R.DocumentChangeValue.SetProject) ours.set('tsugumi.json', parse(String((c.value as R.SetProjectT).json)))
        else if (c.valueType === R.DocumentChangeValue.SetLayout) ours.set('layout.json', parse(String((c.value as R.SetLayoutT).json)))
        else if (c.valueType === R.DocumentChangeValue.PutSource) {
          const p = c.value as R.PutSourceT, bytes = Uint8Array.from(p.bytes)
          if (String(p.path).endsWith('.json')) ours.set(String(p.path), parse(new TextDecoder().decode(bytes)))
          else sources.set(String(p.path), bytes)
        } else assert.fail('Unexpected change')
      }
      const merged = hostDocumentReply({ baseJson: serialize(base), oursJson: serialize(ours), theirsJson: serialize(documents) })
      assert.deepEqual(merged.conflicts, [])
      documents = parse(merged.mergedJson) as JObj
      // The core serializes an accepted project in its document order.
      documents.set('tsugumi.json', new JObj((documents.get('tsugumi.json') as JObj).entries().sort(([a], [b]) => a.localeCompare(b))))
      const delta = Object.assign(new R.DocumentDeltaT(), { baseRev: revision, rev: ++revision, opId, origin,
        projectJson: serialize(documents.get('tsugumi.json')!), layoutJson: serialize(documents.get('layout.json')!) })
      for (const [path, doc] of documents.entries()) if (path.startsWith('externals/')) sources.set(path, new TextEncoder().encode(serialize(doc)))
      delta.sourcesUpsert = [...sources].map(([path, bytes]) => ({ path, hash: createHash('sha256').update(bytes).digest('hex'), sizeBytes: BigInt(bytes.length) })) as never
      await beforeReply()
      return Object.assign(new R.SaveResultT(), { rev: revision, delta })
    },
  } as never
  return { s, sources, documents: () => JSON.parse(serialize(documents)) as Record<string, unknown>,
    beforeReply: (f: () => Promise<void>) => { beforeReply = f }, beforeRead: (f: () => Promise<void>) => { beforeRead = f },
    save: () => (s as unknown as { saveOnce(): Promise<void> }).saveOnce() }
}

// A save of a newly created external is followed by an edit before its body
// loads. A missing cache entry is not a concurrent deletion.
{
  const f = fixture(), { s } = f, prefix = 'externals/sub/'
  const binary = new Uint8Array([0, 255, 128, 195, 40, 10]), nested = prefix + 'externals/child/'
  const external = () => ({ project: parse('{"name":"sub","canvas":{"cards":[],"connections":[]}}'), layout: obj(), definitions: new Map() })
  s.edit(x => {
    x.createExternal(prefix, external()); x.createExternal(nested, external())
    x.sourceEdits.set(prefix + 'asset.bin', binary)
    x.sourceEdits.set(nested + 'asset.bin', binary)
  })
  const reading = deferred(), release = deferred()
  f.beforeRead(async () => { reading.resolve(); await release.promise })
  f.beforeReply(async () => { s.edit(x => x.updateExternal(prefix, e => { (e.project as JObj).set('name', 'renamed') })) })
  const saving = f.save(), next = saving.then(() => f.save())
  await reading.promise
  // Drain acknowledgement continuations while document reads remain blocked.
  await new Promise<void>(resolve => setImmediate(resolve))
  release.resolve()
  await saving; await next
  assert.equal(s.saveError, undefined, 'an unread acknowledged external is not a deletion')
  await f.save()
  assert.equal(s.saveError, undefined)
  assert.equal((f.documents()[prefix + 'tsugumi.json'] as { name: string }).name, 'renamed')
  assert.ok(f.documents()[nested + 'tsugumi.json'])
  assert.deepEqual(f.sources.get(prefix + 'asset.bin'), binary)
  assert.deepEqual(f.sources.get(nested + 'asset.bin'), binary)
}

// Saving can change JSON spelling and draft/confirmed storage while Untangle
// waits. Its scene is unchanged, so apply the proposal as one undoable edit.
for (const action of ['untangle', 'arrange'] as const) {
  const f = fixture(), { s } = f, proposal = deferred(), release = deferred()
  const beforeLayout = serialize(s.layout), graph = serialize(s.project)
  const answer = async () => {
    proposal.resolve(); await release.promise
    return { cards: action === 'arrange' ? [{ id: 'a', x: 100, y: 200 }] : [], frames: [], notes: [], portOrders: [{ id: 'a', inputs: [], outputs: ['b', 'a'] }] }
  }
  s.layoutHost = { validate: async (json: string) => json, layoutUntangle: answer, layoutArrange: answer } as never
  s.edit(x => { (x.project as JObj).set('name', 'mine') })
  const pending = s[action]([])
  await proposal.promise
  await f.save()
  const steps = s.undoStack.length
  release.resolve(); await pending
  assert.deepEqual(get(s.layout, 'cards', 'a', 'outputOrder'), ['b', 'a'])
  assert.equal(s.undoStack.length, steps + 1)
  assert.deepEqual(JSON.parse(serialize(s.project)), JSON.parse(graph.replace('base', 'mine')))
  assert.equal((f.documents()['layout.json'] as any).groups[0].name, 'group')
  const placedLayout = serialize(s.layout)
  await f.save(); await s.undo()
  assert.equal(s.saveError, undefined, action + ': undo: ' + s.saveError)
  assert.equal(get(s.layout, 'cards', 'a', 'outputOrder'), undefined)
  assert.equal(serialize(s.layout), beforeLayout)
  await f.save(); await s.redo()
  assert.deepEqual(get(s.layout, 'cards', 'a', 'outputOrder'), ['b', 'a'])
  assert.equal(serialize(s.layout), placedLayout)
  await f.save()
}
console.log('save acknowledgements preserve external edits and layout proposals: passed')
