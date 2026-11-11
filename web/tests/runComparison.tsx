import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { D, R, cardPath } from '../src/shared/rpc'
import { comparedParams, comparedValues, loadRunComparison, sameDisplay } from '../src/model/runComparison'
import { recordedDisplay } from '../src/model/session'
import { ComparedCardView } from '../src/ui/runComparison'

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
assert.deepEqual(comparedParams(a, b).map(f => [f.name, f.changed]), [['boundary', true], ['low', false]])
assert.deepEqual(comparedParams(undefined, b).map(f => f.left), [undefined, undefined])
const v = Object.assign(new D.NamedValueT(), { name: 'photons', rootType: 'Counts', inlineBytes: [1, 2, 3], sizeBytes: 3n })
a.record!.outputs = [v]; b.record!.outputs = [Object.assign(new D.NamedValueT(), v, { inlineBytes: [1, 2, 4] })]
assert.equal(comparedValues('outputs', a, b)[0].changed, true)
b.record!.outputs = [Object.assign(new D.NamedValueT(), v, { artifactId: 'another-retained-copy' })]
assert.equal(comparedValues('outputs', a, b)[0].changed, false, 'retention identity alone does not change the value')

// Use the real Cas A retained table and images as the rendering fixture.
const demo = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'docs/site/player-data/casa.json'), 'utf8'))
const table = demo.outputs.bands
const display = (runId: string, valueType: D.DisplayValue, value: D.DisplayTableT | D.DisplayImageT) => Object.assign(new D.DisplayT(), { runId, invocation: 1n, revision: 1n, card: cardPath(['bands']), valueType, value })
const tableValue = (rows: string[][]) => Object.assign(new D.DisplayTableT(), { columns: table.columns, rows: rows.map(cells => Object.assign(new D.TableRowT(), { cells })) })
a.record!.display = display('left', D.DisplayValue.DisplayTable, tableValue(table.rows))
const changedRows = structuredClone(table.rows); changedRows[0][2] = '104821'
b.record!.display = display('right', D.DisplayValue.DisplayTable, tableValue(changedRows))
assert.equal(sameDisplay(a.record!.display, b.record!.display), false)
assert.equal(recordedDisplay(a.record!.display)?.value.kind, 'table')
const markup = renderToStaticMarkup(<ComparedCardView row={rows[0]} />)
for (const text of ['bands', '1650', '1800', '101187', '104821', 'Photons']) assert.ok(markup.includes(text), text)
assert.ok(markup.includes('changed'), 'differences have a visible highlight')
for (const name of ['rgb', 'spectrum']) {
  const image = demo.outputs[name]
  const bytes = [...fs.readFileSync(path.join(process.argv[2], 'docs/site/player-data', image.src))]
  const value = Object.assign(new D.DisplayImageT(), { data: bytes, mediaType: name === 'rgb' ? 'image/jpeg' : 'image/png', alt: name })
  const retained = display('left', D.DisplayValue.DisplayImage, value)
  const other = display('right', D.DisplayValue.DisplayImage, value)
  other.revision = 99n
  assert.ok(sameDisplay(retained, other), 'identity and frame revision do not change image equality')
  assert.equal(recordedDisplay(retained)?.value.kind, 'image')
  a.record!.display = retained; b.record!.display = other
  assert.ok(renderToStaticMarkup(<ComparedCardView row={rows[0]} />).includes('<img'), name + ': both retained images render')
}
console.log('run comparison: retained Cas A table/images, params, I/O, missing/failed/stopped Cards and read-only loading passed')
