// A Draft implemented becomes a Card that sends its results, and the session
// asks for them at once: the frames of the Agent's Cards open while they run.
import assert from 'node:assert/strict'
import { parse } from '../src/shared/json'
import * as R from '../src/gen/tsugumi/rpc'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')

const resolved = (id: string) => ({ card: { segments: [id] }, inputs: [], outputs: [], diagnostics: [] }) as unknown as R.ResolvedCardT
const definition = (id: string, card: string) => Object.assign(new R.DefinitionT(), { id, cardJson: card })
const delta = (rev: bigint, fields: Partial<R.DocumentDeltaT>) =>
  Object.assign(new R.DocumentDeltaT(), { baseRev: rev, rev: rev + 1n, resolution: [resolved('a')], diagnostics: [], ...fields })

const session = () => {
  const s = new ProjectSession('/p', 'k', 'p')
  const project = '{"name":"t","canvas":{"cards":[{"id":"a"}],"connections":[]}}'
  s.project = parse(project)
  s.confirmedProject = parse(project)
  s.layout = parse('{}')
  s.confirmedLayout = parse('{}')
  s.confirmedDefinitions.set('a', parse('{"template":"draft"}'))
  s.rev = 1n
  const asked: string[][] = []
  s.client = { setVisible: async (_sub: unknown, cards: string[][]) => { asked.push(cards.map((c) => c.join('/'))) } } as never
  const inner = s as unknown as { subscription: string; applyDelta(d: R.DocumentDeltaT): void; applyResolution(r: R.ResolvedCardT[], d: unknown[]): void }
  inner.subscription = 'sub'
  inner.applyResolution([resolved('a')], [])
  return { s, asked, apply: (d: R.DocumentDeltaT) => inner.applyDelta(d) }
}

{
  const { s, asked, apply } = session()
  assert.equal(s.kind('a'), 'draft')
  apply(delta(1n, { definitionsUpsert: [definition('a', '{"language":"python"}')] }))
  assert.equal(s.kind('a'), 'code')
  assert.deepEqual(asked, [['a']], 'the Draft that became code asks for its displays')
}

// Nothing changed about which Cards send results: nothing is asked again.
{
  const { asked, apply } = session()
  apply(delta(1n, { definitionsUpsert: [definition('a', '{"template":"draft"}')] }))
  assert.deepEqual(asked, [])
}

// A Card that stops being one (code back to a Draft) is left out at once as well.
{
  const { s, asked, apply } = session()
  s.confirmedDefinitions.set('a', parse('{"language":"python"}'))
  apply(delta(1n, { definitionsUpsert: [definition('a', '{"template":"draft"}')] }))
  assert.deepEqual(asked, [[]])
}

console.log('draft displays: passed')
