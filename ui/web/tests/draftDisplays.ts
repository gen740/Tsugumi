// Only open live viewers subscribe to displays.
import assert from 'node:assert/strict'
import * as R from '../shared/gen/tsugumi/rpc'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')

const resolved = (id: string) => ({ card: { segments: [id] }, inputs: [], outputs: [], diagnostics: [], nixReferences: [] }) as unknown as R.ResolvedCardT
const definition = (id: string, card: string) => Object.assign(new R.DefinitionT(), { id, cardJson: card })
const delta = (rev: bigint, fields: Partial<R.DocumentDeltaT>) =>
  Object.assign(new R.DocumentDeltaT(), { baseRev: rev, rev: rev + 1n, resolution: [resolved('a')], diagnostics: [], ...fields })

const session = () => {
  const s = new ProjectSession('/p', 'k', 'p')
  const project = '{"name":"t","canvas":{"cards":[{"id":"a"}],"connections":[]}}'
  const asked: string[][] = []
  s.client = { setVisible: async (_sub: unknown, cards: string[][]) => { asked.push(cards.map((c) => c.join('/'))) } } as never
  const inner = s as unknown as { subscription: string; applyDelta(d: R.DocumentDeltaT): void; apply(snapshot: R.ProjectSnapshotT, reset: boolean): void }
  inner.subscription = 'sub'
  inner.apply(Object.assign(new R.ProjectSnapshotT(), { rev: 1n, projectJson: project, layoutJson: '{}',
    definitions: [definition('a', '{"type":"draft"}')], resolution: [resolved('a')] }))
  return { s, asked, apply: (d: R.DocumentDeltaT) => inner.applyDelta(d) }
}

const { s, asked, apply } = session()
apply(delta(1n, { definitionsUpsert: [definition('a', '{"type":"code","language":"python"}')] }))
assert.equal(s.kind('a'), 'code')
assert.deepEqual(asked, [], 'the canvas does not subscribe when a Draft becomes code')

const closeA = s.observeDisplay('a')
const closeSecondA = s.observeDisplay('a')
const closeB = s.observeDisplay('nested/b')
closeA()
closeB()
closeSecondA()
assert.deepEqual(asked, [['a'], ['a', 'nested/b'], ['a'], []])

console.log('live display subscriptions: passed')
