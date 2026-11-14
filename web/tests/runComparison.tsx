import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { D, R, cardPath } from '../src/shared/rpc'
import { comparedConditions, comparedParams, comparedValues, loadRunComparison, runScope, sameDisplay, valueDifference } from '../src/model/runComparison'
import { ProjectSession, recordedDisplay } from '../src/model/session'
import { ComparedCardView, RunComparisonView, RunScopes, VersionDifference } from '../src/ui/runComparison'

const summary = (runId: string, card: string, state = D.CardState.succeeded) => Object.assign(new R.InvocationSummaryT(), {
  runId, card: cardPath(card.split('/')), invocation: 1n, state, durationMs: 10n,
})
const record = (runId: string, card: string, params: string, state = D.CardState.succeeded) => Object.assign(new R.RecordResultT(), {
  record: Object.assign(new R.InvocationRecordT(), { summary: summary(runId, card, state), paramsJson: params, recordIo: true }),
  status: Object.assign(new R.HistoryStateT(), { state }), aggregate: Object.assign(new R.AggregateT(), { calls: 1n, succeeded: 1n }), exists: true,
})
const a = record('left', 'bands', '{"low":600,"boundary":1650}')
const b = record('right', 'bands', '{"low":600,"boundary":1800}')
const calls: string[] = []
const rows = await loadRunComparison({
  async latestInvocations(run) { calls.push('latest:' + run); return Object.assign(new R.InvocationPageT(), {
    records: run === 'left' ? [a.record!.summary!, summary(run, 'removed')] : [b.record!.summary!, summary(run, 'failed', D.CardState.failed), summary(run, 'stopped', D.CardState.cancelled)],
  }) },
  async record(run, card, invocation, follow) {
    assert.equal(invocation, 1n); assert.equal(follow, false)
    calls.push('record:' + run + '/' + card.join('/'))
    if (card[0] === 'stopped') throw new Error('record unavailable')
    return card[0] === 'bands' ? run === 'left' ? a : b : record(run, card[0], '{}', card[0] === 'failed' ? D.CardState.failed : D.CardState.succeeded)
  },
}, 'left', 'right')
assert.deepEqual(rows.map(r => r.card), ['bands', 'failed', 'removed', 'stopped'])
assert.equal(rows[1].left, undefined); assert.equal(rows[2].right, undefined)
assert.match(rows[3].right!.error!, /record unavailable/)
assert.equal(rows[1].right!.summary.state, D.CardState.failed)
assert.equal(rows[3].right!.summary.state, D.CardState.cancelled)
assert.ok(calls.every(c => /^(latest|record):/.test(c)), 'comparison only reads the selected runs')
assert.deepEqual(comparedParams(a, b).map(f => [f.name, f.difference]), [['boundary', 'changed'], ['low', 'same']])
assert.deepEqual(comparedParams(undefined, b).map(f => [f.left, f.difference]), [[undefined, 'changed'], [undefined, 'changed']])
// A record that holds no parameters says nothing about them: not a list of removed ones.
const unrecorded = record('left', 'bands', '{}'); unrecorded.record!.paramsJson = null
assert.deepEqual(comparedParams(unrecorded, b).map(f => f.difference), ['unknown', 'unknown'])
const v = Object.assign(new D.NamedValueT(), { name: 'photons', rootType: 'Counts', inlineBytes: [1, 2, 3], sizeBytes: 3n })
a.record!.outputs = [v]; b.record!.outputs = [Object.assign(new D.NamedValueT(), v, { inlineBytes: [1, 2, 4] })]
assert.equal(comparedValues('outputs', a, b)[0].difference, 'changed')
b.record!.outputs = [Object.assign(new D.NamedValueT(), v, { artifactId: 'another-retained-copy' })]
assert.equal(comparedValues('outputs', a, b)[0].difference, 'same', 'retention identity alone does not change the value')

// A value the response left out (too large to inline) is never reported as unchanged.
const omitted = (hash: string | null = null, size = 9000000n) => Object.assign(new D.NamedValueT(), { name: 'photons', rootType: 'Counts', inlineBytes: [], sizeBytes: size, contentHash: hash })
assert.equal(valueDifference(omitted(), omitted()), 'unknown', 'equal size cannot tell if the values match')
assert.equal(valueDifference(omitted(), omitted(null, 9000001n)), 'unknown', 'payloads and digests were omitted')
assert.equal(valueDifference(omitted('aa'), omitted('aa')), 'same', 'the recorded digest settles it')
assert.equal(valueDifference(omitted('aa'), omitted('bb')), 'changed')
assert.equal(valueDifference(omitted('aa'), omitted()), 'unknown', 'one digest alone settles nothing')
assert.equal(valueDifference(v, omitted(null, 3n)), 'unknown', 'inline on one side only')
assert.equal(valueDifference(v, Object.assign(new D.NamedValueT(), v, { rootType: 'Other' })), 'changed', 'a type change is a difference')
assert.equal(valueDifference(v, undefined), 'changed')
{
  const x = record('left', 'big', '{}'), y = record('right', 'big', '{}')
  x.record!.outputs = [omitted()]; y.record!.outputs = [omitted()]
  x.truncated = y.truncated = true
  const field = comparedValues('outputs', x, y)[0]
  assert.equal(field.left, field.right, 'the two summaries read the same')
  assert.equal(field.difference, 'unknown')
  const html = renderToStaticMarkup(<ComparedCardView row={{ card: 'big', left: { summary: x.record!.summary!, record: x }, right: { summary: y.record!.summary!, record: y } }} />)
  assert.ok(html.includes('comparison-field unknown') && html.includes('比較できません'), 'an omitted value is marked not comparable')
}

// Execution conditions beyond parameters: environment, service, emulation and input sources.
{
  const side = (run: string, env: string | null, emulator: string | null, executionService: string | null = null) => {
    const r = record(run, 'bands', '{}')
    Object.assign(r.record!.summary!, { environmentId: env, emulator, executionService })
    return { summary: r.record!.summary!, record: r }
  }
  const source = (port: string, card: string, invocation: bigint) => Object.assign(new R.InputSourceT(), { port, card: cardPath([card]), invocation })
  const value = (name: string) => Object.assign(new D.NamedValueT(), { name, rootType: 'Events', inlineBytes: [1], sizeBytes: 1n })
  const x = side('left', 'env-a', null, 'local'), y = side('right', 'env-b', 'qemu-x86_64', 'local')
  // Left ran after `load` in the same run; right ran alone on the value an earlier run saved.
  x.record.record!.inputs = [value('events')]; x.record.record!.inputSources = [source('events', 'load', 2n)]
  y.record.record!.inputs = [value('events')]
  const fields = comparedConditions(x, y)
  assert.deepEqual(fields.map(f => f.difference), ['changed', 'changed', 'same', 'changed'])
  assert.deepEqual([fields[0].left, fields[0].right], ['env-a', 'env-b'])
  assert.deepEqual([fields[1].left, fields[1].right], ['直接実行', 'エミュレーション: qemu-x86_64'])
  assert.deepEqual([fields[2].name, fields[2].left, fields[2].right], ['実行サービス', 'プロジェクトの実行環境', 'プロジェクトの実行環境'])
  assert.deepEqual([fields[3].name, fields[3].left, fields[3].right], ['入力元 events', 'load #2', '保存した入力'])
  // Without recorded inputs a saved value cannot be told from no value: say only what is known.
  y.record.record!.inputs = []
  assert.equal(comparedConditions(x, y)[3].right, 'この実行の Card からは届いていません')
  assert.deepEqual(comparedConditions(x, x).map(f => f.difference), ['same', 'same', 'same', 'same'])
  assert.deepEqual(comparedConditions(x, side('older', 'env-a', null)).slice(1, 3).map(f => [f.right, f.difference]), [[undefined, 'unknown'], [undefined, 'unknown']])
  // Missing environment records on either side cannot establish equality.
  assert.equal(comparedConditions(x, side('right', null, null))[0].difference, 'unknown')
  assert.equal(comparedConditions(side('left', null, null), side('right', null, null))[0].difference, 'unknown')
  assert.equal(comparedConditions(x, undefined)[0].difference, 'changed')
  const html = renderToStaticMarkup(<ComparedCardView row={{ card: 'bands', left: x, right: y }} />)
  for (const text of ['実行条件', '環境が変わった Card', 'env-a', 'env-b', 'qemu-x86_64', 'プロジェクトの実行環境', 'load #2']) assert.ok(html.includes(text), text)
}

// Run scope: where each run started and whether it went on downstream.
{
  const run = (target: string, propagate: boolean | null) => Object.assign(new R.RunSummaryT(), { target: cardPath([target]), propagate })
  assert.equal(runScope(run('start', true)), 'start から下流')
  assert.equal(runScope(run('start', null)), 'start から下流', 'propagate defaults to true')
  assert.equal(runScope(run('bands', false)), 'bands だけ（保存した入力）')
  const sharedName = () => 'Same name'
  const sameNames = renderToStaticMarkup(<RunScopes left={run('start', true)} right={run('bands', true)} displayPath={sharedName} />)
  assert.ok(sameNames.includes('Same name') && sameNames.includes('comparison-field changed'), 'names do not decide identity')
  assert.ok(!renderToStaticMarkup(<RunScopes left={run('start', true)} right={run('start', null)} displayPath={sharedName} />).includes('changed'))
  const html = renderToStaticMarkup(<RunScopes left={run('start', true)} right={run('bands', false)} />)
  assert.ok(html.includes('comparison-field changed') && html.includes('bands だけ（保存した入力）'))
  assert.ok(!renderToStaticMarkup(<RunScopes left={run('start', true)} right={run('start', true)} />).includes('changed'))
}

// A Card called several times in a run is compared by its last call, and says so.
{
  const many = record('left', 'loop', '{}')
  many.record!.summary!.invocation = 3n; many.aggregate!.calls = 3n
  const once = record('right', 'loop', '{}')
  const html = renderToStaticMarkup(<ComparedCardView row={{ card: 'loop', left: { summary: many.record!.summary!, record: many }, right: { summary: once.record!.summary!, record: once } }} />)
  assert.equal(html.split('回の呼び出しのうち最後の 1 回').length - 1, 1, 'only the side called more than once is labelled')
  assert.ok(html.includes('3 回の呼び出しのうち最後の 1 回'))
}

// Use the real Cas A retained table and images as the rendering fixture.
const demo = JSON.parse(fs.readFileSync(path.join(process.env.TSUGUMI_SITE_PLAYER_DIR!, 'player-data/casa.json'), 'utf8'))
const table = demo.outputs.bands
const display = (runId: string, valueType: D.DisplayValue, value: D.DisplayTableT | D.DisplayImageT) => Object.assign(new D.DisplayT(), { runId, invocation: 1n, revision: 1n, card: cardPath(['bands']), valueType, value })
const tableValue = (rows: string[][]) => Object.assign(new D.DisplayTableT(), { columns: table.columns, rows: rows.map(cells => Object.assign(new D.TableRowT(), { cells })) })
a.record!.display = display('left', D.DisplayValue.DisplayTable, tableValue(table.rows))
const changedRows = structuredClone(table.rows); changedRows[0][2] = '104821'
b.record!.display = display('right', D.DisplayValue.DisplayTable, tableValue(changedRows))
const shown = (d?: D.DisplayT | null) => d ? recordedDisplay(d) : undefined
assert.equal(sameDisplay(shown(a.record!.display), shown(b.record!.display)), false)
assert.equal(sameDisplay(shown(a.record!.display), undefined), false)
assert.equal(sameDisplay(undefined, undefined), true)
{
  // The same rule as the Mac app: compare what is shown, kind by kind; module equality includes binary data.
  const chart = (y: number[]) => ({ runId: 'r', invocation: 1n, revision: 1n, value: { kind: 'module' as const, name: 'line-chart', json: '{}', blobs: { y: new Uint8Array(new Float64Array(y).buffer) } } })
  assert.ok(sameDisplay(chart([1, NaN]), chart([1, NaN])))
  assert.ok(!sameDisplay(chart([1, NaN]), chart([1, 2])))
  const text = (kind: 'text' | 'markdown', value: string) => ({ runId: 'r', invocation: 1n, revision: 1n, value: { kind, text: value } })
  assert.ok(!sameDisplay(text('text', 'a'), text('markdown', 'a')), 'plain text and Markdown are different displays')
  const scalar = (value: number, label: string) => ({ runId: 'r', invocation: 1n, revision: 1n, value: { kind: 'scalar' as const, value, label } })
  assert.ok(sameDisplay(scalar(1, 'n'), scalar(1, 'n')) && !sameDisplay(scalar(1, 'n'), scalar(1, 'm')))
}
assert.equal(recordedDisplay(a.record!.display)?.value.kind, 'table')
const markup = renderToStaticMarkup(<ComparedCardView row={rows[0]} />)
for (const text of ['bands', '1650', '1800', '101187', '104821', 'Photons']) assert.ok(markup.includes(text), text)
assert.ok(markup.includes('changed'), 'differences have a visible highlight')
for (const name of ['rgb', 'spectrum']) {
  const image = demo.outputs[name]
  const bytes = [...fs.readFileSync(path.join(process.env.TSUGUMI_SITE_PLAYER_DIR!, 'player-data', image.src))]
  const value = Object.assign(new D.DisplayImageT(), { data: bytes, mediaType: name === 'rgb' ? 'image/jpeg' : 'image/png', alt: name })
  const retained = display('left', D.DisplayValue.DisplayImage, value)
  const other = display('right', D.DisplayValue.DisplayImage, value)
  other.revision = 99n
  assert.ok(sameDisplay(shown(retained), shown(other)), 'identity and frame revision do not change image equality')
  assert.equal(recordedDisplay(retained)?.value.kind, 'image')
  a.record!.display = retained; b.record!.display = other
  assert.ok(renderToStaticMarkup(<ComparedCardView row={rows[0]} />).includes('<img'), name + ': both retained images render')
}
// Opening a comparison leaves the live canvas alone: the displays the session holds are
// the same objects afterwards, and the only requests are reads of the two selected runs.
{
  const session = new ProjectSession('/p', 'k', 'p')
  const live = recordedDisplay(display('current', D.DisplayValue.DisplayTable, tableValue(table.rows)))!
  session.displays.set('bands', live)
  const requests: string[] = []
  const fake = new Proxy({}, { get: (_, name: string) => async (...args: unknown[]) => {
    requests.push(name)
    if (name === 'latestInvocations') return Object.assign(new R.InvocationPageT(), { records: [summary(args[0] as string, 'bands')] })
    if (name === 'record') { assert.equal(args[3], false, 'never follows the live run'); return args[0] === 'left' ? a : b }
    throw new Error('unexpected request ' + name)
  } })
  ;(session as unknown as { client: unknown }).client = fake
  const before = new Map(session.displays)
  const loaded = await loadRunComparison(session.rpc!, 'left', 'right')
  const run = (id: string) => Object.assign(new R.RunSummaryT(), { status: Object.assign(new R.RunStatusT(), { runId: id, state: D.RunState.succeeded }), target: cardPath(['start']) })
  renderToStaticMarkup(<RunComparisonView session={session} runs={[run('left'), run('right')]} onClose={() => {}} />)
  for (const row of loaded) renderToStaticMarkup(<ComparedCardView row={row} />)
  assert.deepEqual(requests.sort(), ['latestInvocations', 'latestInvocations', 'record', 'record'])
  assert.equal(session.displays.size, before.size)
  assert.equal(session.displays.get('bands'), live, 'the canvas keeps its own display object')
  assert.equal(live.runId, 'current')
}

// Versions: a run that recorded none, the same version, an empty diff and a diff cut short.
{
  const diff = (text: string, truncated = false) => Object.assign(new R.VersionDiffT(), { text, truncated })
  const html = (props: Parameters<typeof VersionDifference>[0]) => renderToStaticMarkup(<VersionDifference {...props} />)
  assert.ok(html({ from: '', to: 'b'.repeat(40) }).includes('版が記録されていないため差分を表示できません。'), 'no version on one side')
  assert.ok(html({ from: '', to: '' }).includes('版が記録されていないため差分を表示できません。'))
  assert.ok(html({ from: 'a'.repeat(40), to: 'a'.repeat(40) }).includes('同じ版です'))
  assert.ok(html({ from: 'a'.repeat(40), to: 'b'.repeat(40), diff: diff('') }).includes('変更はありません'))
  const cut = html({ from: 'a'.repeat(40), to: 'b'.repeat(40), diff: diff('+boundary = 1800', true) })
  assert.ok(cut.includes('+boundary = 1800') && cut.includes('差分が大きいため一部を表示しています。'), 'a truncated diff says so')
  assert.ok(!html({ from: 'a'.repeat(40), to: 'b'.repeat(40), diff: diff('+x') }).includes('差分が大きいため'))
  assert.ok(html({ from: 'a'.repeat(40), to: 'b'.repeat(40), error: 'not_found' }).includes('not_found'))
}

console.log('run comparison: retained Cas A table/images, params, conditions, scope, omitted values, versions, I/O, missing/failed/stopped Cards and read-only loading passed')
