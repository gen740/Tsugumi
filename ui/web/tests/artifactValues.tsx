// Artifact ids are uuid text, shown with their `N.k` label; values say what each
// id resolves to now, and the artifact panel lists available ones.
import assert from 'node:assert/strict'
import { setImmediate } from 'node:timers/promises'
import { Builder } from 'flatbuffers'
import { act, create } from 'react-test-renderer'
import { BaseType } from '../shared/bfbs'
import { ArtifactT } from '../shared/gen/tsugumi/artifact'
import { ArtifactIdT } from '../shared/gen/tsugumi/artifact-id'
import { L } from '../src/i18n'
import { parseEvidence } from '../src/model/evidence'
import { ProjectSession } from '../src/model/session'
import { resolveEvidence } from '../src/ui/evidence'
import { CardArtifactsView } from '../src/ui/history'
import { artifactName, artifactStateText, describeNamedValue, describeValue, uuidText } from '../src/wire/flatjson'
import { D, R, cardPath } from '../src/wire/rpc'

const uuid = '0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0'
const uuidBytes = uuid.replaceAll('-', '').match(/../g)!.map(h => parseInt(h, 16))
assert.equal(uuidText(Uint8Array.from(uuidBytes)), uuid)

/** A reflection schema with `tsugumi.Artifact { id: tsugumi.ArtifactId }` and the 16-byte struct. */
function artifactSchema(): number[] {
  const b = new Builder(256)
  const offsets = (items: number[]) => {
    b.startVector(4, items.length, 4)
    for (const x of [...items].reverse()) b.addOffset(x)
    return b.endVector()
  }
  const type = (base: BaseType, element: BaseType, index: number, fixedLength = 0) => {
    b.startObject(6)
    b.addFieldInt8(0, base, 0); b.addFieldInt8(1, element, 0); b.addFieldInt32(2, index, -1); b.addFieldInt16(3, fixedLength, 0)
    return b.endObject()
  }
  const field = (name: string, t: number, id: number, offset: number) => {
    const n = b.createString(name)
    b.startObject(12)
    b.addFieldOffset(0, n, 0); b.addFieldOffset(1, t, 0); b.addFieldInt16(2, id, 0); b.addFieldInt16(3, offset, 0)
    return b.endObject()
  }
  const object = (name: string, fields: number[], isStruct: boolean, byteSize: number) => {
    const n = b.createString(name)
    const v = offsets(fields)
    b.startObject(8)
    b.addFieldOffset(0, n, 0); b.addFieldOffset(1, v, 0); b.addFieldInt8(2, isStruct ? 1 : 0, 0); b.addFieldInt32(3, 1, 0); b.addFieldInt32(4, byteSize, 0)
    return b.endObject()
  }
  const table = object('tsugumi.Artifact', [field('id', type(BaseType.obj, BaseType.none, 1), 0, 4)], false, 0)
  const struct = object('tsugumi.ArtifactId', [field('bytes', type(BaseType.array, BaseType.ubyte, -1, 16), 0, 0)], true, 16)
  const objects = offsets([table, struct])
  const enums = offsets([])
  b.startObject(2); b.addFieldOffset(0, objects, 0); b.addFieldOffset(1, enums, 0)
  b.finish(b.endObject())
  return Array.from(b.asUint8Array())
}
const value = () => {
  const b = new Builder(64)
  b.finish(new ArtifactT(new ArtifactIdT(uuidBytes)).pack(b))
  return Array.from(b.asUint8Array())
}

// The decoder sees the ArtifactId struct by name and shows its uuid, not a byte array.
{
  const text = describeValue({ name: 'figure', rootType: 'tsugumi.Artifact', sizeBytes: null, inlineBytes: value(), schemaBfbs: artifactSchema() })
  assert.ok(text.includes(`"id": "${uuid}"`), text)
}

// Each artifact of a value carries its label and its state now.
{
  const named = (state: D.ArtifactState, label: string | null) => Object.assign(new D.NamedValueT(), {
    name: 'figure', rootType: 'tsugumi.Artifact', sizeBytes: 20n, inlineBytes: value(), schemaBfbs: artifactSchema(),
    artifacts: [new D.ValueArtifactT(uuid, 4096n, label, state)],
  })
  const available = describeNamedValue(named(D.ArtifactState.available, '3.1'))
  assert.ok(available.includes(L('成果物 {0}', artifactName(uuid, '3.1'))), available)
  assert.ok(available.includes(`${uuid} (3.1)`))
  assert.ok(available.includes(artifactStateText(D.ArtifactState.available)))
  assert.ok(available.includes('4096 bytes'))
  const unavailable = describeNamedValue(named(D.ArtifactState.unavailable, null))
  assert.ok(unavailable.includes(L('成果物 {0}', uuid)))
  assert.ok(unavailable.includes(artifactStateText(D.ArtifactState.unavailable)))
}

// The panel lists kept artifacts and artifacts that will expire.
{
  const info = (id: string, label: string, keep: boolean) => Object.assign(new R.ArtifactInfoT(), {
    artifactId: id, runId: 'run', card: cardPath(['plot']), invocation: 3n, label, keep,
    sizeBytes: 10n, members: [Object.assign(new R.ArtifactMemberT(), { path: 'plot.txt', sizeBytes: 10n })],
  })
  const kept = info(uuid, '3.1', true)
  const scratch = info('11111111-2222-3333-4444-555555555555', '3.2', false)
  const reads: string[] = []
  const session = {
    subscribe: () => () => {}, connected: true, runActive: false, lastFinished: 0, recordingRevision: 0,
    rpc: {
      history: async () => Object.assign(new R.RunPageT(), { runs: [Object.assign(new R.RunSummaryT(), { status: Object.assign(new R.RunStatusT(), { runId: 'run', acceptedAt: 0n, state: D.RunState.succeeded }) })] }),
      artifacts: async () => [kept, scratch],
      readArtifact: async (id: string) => { reads.push(id); return new Uint8Array([104, 105]) },
    },
  } as unknown as ProjectSession
  let view!: ReturnType<typeof create>
  await act(async () => { view = create(<CardArtifactsView session={session} cardKey="plot" />); await setImmediate() })
  await act(async () => { await setImmediate() })
  try {
    const texts = () => view.root.findAll(node => typeof node.type === 'string').flatMap(node => node.children.filter((c): c is string => typeof c === 'string'))
    const all = texts().join('\n')
    for (const a of [kept, scratch]) assert.ok(all.includes(artifactName(String(a.artifactId), String(a.label))), all)
    assert.ok(all.includes(L('残さない（実行の終了時に削除）')))
    assert.ok(all.includes(L('書き出す…')))
    const row = view.root.findAll(node => node.props.role === 'option' && node.props['aria-selected'] === false)[0]
    await act(async () => { row.props.onClick(); await setImmediate() })
    assert.ok(reads.length > 0, 'an available artifact shows its files')
  } finally { view.unmount() }
}

// An `artifact:` link resolves by uuid: available in this project.
{
  const found = (runId = '12') => Object.assign(new R.ArtifactInfoT(), {
    artifactId: uuid, runId, card: cardPath(['plot']), members: [Object.assign(new R.ArtifactMemberT(), { path: 'plot.png' })],
  })
  const lookups: string[][] = []
  const session = (artifact?: R.ArtifactInfoT) => ({
    rpc: {
      call: async () => Object.assign(new R.RunStatusT(), { runId: '12' }),
      artifactsById: async (ids: string[]) => { lookups.push(ids); return artifact ? [artifact] : [] },
    },
  }) as unknown as ProjectSession
  const link = parseEvidence(`artifact:${uuid}?path=plot.png`)!
  assert.equal(await resolveEvidence(session(found()), link), true)
  assert.deepEqual(lookups.at(-1), [uuid])
  assert.equal(await resolveEvidence(session(found('13')), link), true, 'the UUID is independent of the run')
  assert.equal(await resolveEvidence(session(), link), false, 'an unknown or not-kept id')
}
console.log('artifact ids, states and panel: passed')
