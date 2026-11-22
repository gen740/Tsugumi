import assert from 'node:assert/strict'
import { setImmediate } from 'node:timers/promises'
import { L } from '../src/i18n'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { act, create } from 'react-test-renderer'
import { D, R, cardPath } from '../src/wire/rpc'
import { batchMembers, comparedConditions, comparedParams, comparedValues, inBatch, loadRunComparison, memberName, pairMembers, runScope, sameDisplay, valueDifference } from '../src/model/runComparison'
import { ProjectSession } from '../src/model/session'
import { ComparedCardView, ComparedMembers, RunComparisonView, RunScopes, VersionDifference } from '../src/ui/runComparison'

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
const compared = new Map(rows.map(row => [row.card, row]))
assert.deepEqual(new Set(compared.keys()), new Set(['bands', 'failed', 'removed', 'stopped']))
assert.equal(compared.get('failed')!.left, undefined)
assert.equal(compared.get('removed')!.right, undefined)
assert.equal(compared.get('stopped')!.right?.error, 'record unavailable')
assert.equal(compared.get('failed')!.right?.summary.state, D.CardState.failed)
assert.equal(compared.get('stopped')!.right?.summary.state, D.CardState.cancelled)
assert.ok(calls.every(call => /^(latest|record):(left|right)(\/|$)/.test(call)), 'comparison only reads the selected runs')
const byName = <T extends { name: string }>(fields: T[], name: string) => {
  const field = fields.find(field => field.name === name)
  assert.ok(field, `Missing field ${name}`)
  return field
}
assert.equal(byName(comparedParams(a, b), 'boundary').difference, 'changed')
assert.equal(byName(comparedParams(a, b), 'low').difference, 'same')
for (const name of ['boundary', 'low']) {
  const field = byName(comparedParams(undefined, b), name)
  assert.equal(field.left, undefined)
  assert.equal(field.difference, 'changed')
}
// A record that holds no parameters says nothing about them: not a list of removed ones.
const unrecorded = record('left', 'bands', '{}'); unrecorded.record!.paramsJson = null
for (const name of ['low', 'boundary']) assert.equal(byName(comparedParams(unrecorded, b), name).difference, 'unknown')
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
assert.equal(valueDifference(v, undefined), 'unknown')
{
  const x = record('left', 'big', '{}'), y = record('right', 'big', '{}')
  x.record!.outputs = [omitted()]; y.record!.outputs = [omitted()]
  x.truncated = y.truncated = true
  const field = comparedValues('outputs', x, y)[0]
  assert.equal(field.left, field.right, 'the two summaries read the same')
  assert.equal(field.difference, 'unknown')
  const view = create(<ComparedCardView row={{ card: 'big', left: { summary: x.record!.summary!, record: x }, right: { summary: y.record!.summary!, record: y } }} />)
  try {
    const label = view.root.findAllByType('b').find(node => node.children.includes('photons'))
    assert.ok(label?.findAllByType('span').some(mark => mark.children.length > 0), 'the value is identified as not comparable')
    assert.ok(field.left && view.root.findAllByType('pre').some(value => value.props.children === field.left))
  } finally { view.unmount() }
}

// Files compare their recorded content, never the serialized storage path.
{
  const file = (path: string, digest: string | null) => Object.assign(new D.NamedValueT(), {
    name: 'file', kind: D.PortKind.File, rootType: 'File', schemaBfbs: [1],
    inlineBytes: [...new TextEncoder().encode(path)], contentHash: path, fileContentHash: digest,
  })
  const x = file('/runs/left/value', 'bytes'), y = file('/runs/right/value', 'bytes')
  assert.equal(valueDifference(x, y), 'same')
  y.fileContentHash = 'other'; assert.equal(valueDifference(x, y), 'changed')
  y.fileContentHash = null; assert.equal(valueDifference(x, y), 'unknown')
  assert.equal(valueDifference(file('/same/path', null), file('/same/path', null)), 'unknown')
  y.fileContentHash = 'bytes'; y.fileMissing = true; assert.equal(valueDifference(x, y), 'same')
  y.rootType = 'Other'; assert.equal(valueDifference(x, y), 'changed')
  assert.equal(valueDifference(x, undefined), 'unknown')
  assert.equal(valueDifference(undefined, x), 'unknown')
  assert.equal(valueDifference(undefined, undefined), 'same')
  for (const side of ['inputs', 'outputs'] as const) {
    const left = record('left', 'files', '{}'), right = record('right', 'files', '{}')
    left.record![side] = [x]
    assert.equal(comparedValues(side, left, right)[0].difference, 'unknown')
    assert.equal(comparedValues(side, undefined, left)[0].difference, 'unknown')
  }
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
  const environment = byName(fields, L('環境'))
  assert.deepEqual([environment.left, environment.right, environment.difference], ['env-a', 'env-b', 'changed'])
  const method = byName(fields, L('実行方式'))
  assert.equal(method.difference, 'changed')
  assert.ok(method.right?.includes('qemu-x86_64'))
  const service = byName(fields, L('実行サービス'))
  assert.ok(service.left && service.left === service.right)
  assert.equal(service.difference, 'same')
  const inputName = L('入力元 {0}', 'events')
  const input = byName(fields, inputName)
  assert.equal(input.left, 'load #2')
  assert.ok(input.right && input.right !== input.left)
  assert.equal(input.difference, 'changed')
  // Without recorded inputs a saved value cannot be told from no value.
  y.record.record!.inputs = []
  assert.notEqual(byName(comparedConditions(x, y), inputName).right, input.right)
  assert.ok(fields.every(field => byName(comparedConditions(x, x), field.name).difference === 'same'))
  for (const name of [L('実行方式'), L('実行サービス')]) {
    const field = byName(comparedConditions(x, side('older', 'env-a', null)), name)
    assert.equal(field.right, undefined)
    assert.equal(field.difference, 'unknown')
  }
  assert.equal(byName(comparedConditions(x, side('right', null, null)), L('環境')).difference, 'unknown')
  assert.equal(byName(comparedConditions(side('left', null, null), side('right', null, null)), L('環境')).difference, 'unknown')
  assert.equal(byName(comparedConditions(x, undefined), L('環境')).difference, 'changed')
  const html = renderToStaticMarkup(<ComparedCardView row={{ card: 'bands', left: x, right: y }} />)
  for (const text of ['env-a', 'env-b', 'qemu-x86_64', 'load #2']) assert.ok(html.includes(text), text)
}

// Run scope: where each run started and whether it went on downstream.
{
  const run = (target: string, propagate: boolean | null) => Object.assign(new R.RunSummaryT(), { target: cardPath([target]), propagate })
  assert.ok(runScope(run('start', true)).includes('start'))
  assert.equal(runScope(run('start', null)), runScope(run('start', true)), 'propagate defaults to true')
  assert.ok(runScope(run('bands', false)).includes('bands'))
  const sharedName = () => 'Same name'
  const check = (left: R.RunSummaryT, right: R.RunSummaryT, changed: boolean) => {
    const view = create(<RunScopes left={left} right={right} displayPath={sharedName} />)
    try {
      assert.ok(view.root.findAllByType('pre').every(value => String(value.props.children).includes(sharedName())))
      assert.equal(view.root.findByType('b').findAllByType('span').some(mark => mark.children.length > 0), changed)
    } finally { view.unmount() }
  }
  check(run('start', true), run('bands', true), true)
  check(run('start', true), run('start', null), false)
  check(run('start', true), run('start', false), true)
  check(run('start', true), run('start', true), false)
}

// A Card called several times in a run is compared by its last call, and says so.
{
  const many = record('left', 'loop', '{}')
  many.record!.summary!.invocation = 3n; many.aggregate!.calls = 3n
  const once = record('right', 'loop', '{}')
  const html = renderToStaticMarkup(<ComparedCardView row={{ card: 'loop', left: { summary: many.record!.summary!, record: many }, right: { summary: once.record!.summary!, record: once } }} />)
  assert.ok(html.includes('#3') && html.includes('#1'), 'the two sides identify the compared invocations')
}

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
// Opening a comparison leaves the live viewer alone: the displays the session holds are
// the same objects afterwards, and the only requests are reads of the two selected runs.
{
  const session = new ProjectSession('/p', 'k', 'p')
  const live = { runId: 'current', invocation: 1n, revision: 1n, value: { kind: 'text' as const, text: 'live' } }
  session.displays.set('bands', live)
  const requests: string[] = []
  const unavailable = 'fixture history error'
  let fail = true
  const fake = new Proxy({}, { get: (_, name: string) => async (...args: unknown[]) => {
    if (name === 'latestInvocations' && args[0] === 'left' && fail) throw new Error(unavailable)
    requests.push([name, ...args.slice(0, 2)].map(value => String(value)).join(':'))
    if (name === 'latestInvocations') return Object.assign(new R.InvocationPageT(), { records: [summary(args[0] as string, 'bands')] })
    if (name === 'record') { assert.equal(args[3], false, 'never follows the live run'); return args[0] === 'left' ? a : b }
    throw new Error('unexpected request ' + name)
  } })
  ;(session as unknown as { client: unknown }).client = fake
  const before = new Map(session.displays)
  const run = (id: string) => Object.assign(new R.RunSummaryT(), { status: Object.assign(new R.RunStatusT(), { runId: id, state: D.RunState.succeeded }), target: cardPath(['start']) })
  let view!: ReturnType<typeof create>
  act(() => { view = create(<RunComparisonView session={session} runs={[run('left'), run('right')]} onClose={() => {}} />) })
  await setImmediate()
  const warning = view.root.findByProps({ role: 'alert' })
  assert.ok(warning.children.includes(unavailable))
  requests.length = 0
  fail = false
  act(() => { warning.findByType('button').props.onClick({ stopPropagation() {} }) })
  await setImmediate()
  assert.equal(view.root.findAllByProps({ role: 'alert' }).length, 0)
  const row = view.root.findAllByType(ComparedCardView).find(card => card.props.row.card === 'bands')?.props.row
  assert.equal(row?.left?.record, a)
  assert.equal(row?.right?.record, b)
  assert.deepEqual(new Set(requests), new Set(['latestInvocations:left', 'latestInvocations:right', 'record:left:bands', 'record:right:bands']))
  view.unmount()
  assert.equal(session.displays.size, before.size)
  assert.equal(session.displays.get('bands'), live, 'the live viewer keeps its own display object')
  assert.equal(live.runId, 'current')
}

// Versions: a run that recorded none, the same version, an empty diff and a diff cut short.
{
  const diff = (text: string, truncated = false) => ({ text, truncated })
  const versions = (props: Parameters<typeof VersionDifference>[0]) => create(<VersionDifference {...props} />)
  const from = 'a'.repeat(40), to = 'b'.repeat(40)
  const status = (props: Parameters<typeof VersionDifference>[0]) => {
    const view = versions(props)
    try {
      assert.equal(view.root.findAllByType('pre').length, 0)
      return view.root.findByType('p').props.children
    } finally { view.unmount() }
  }
  const missing = status({ from: '', to })
  assert.ok(missing)
  assert.equal(status({ from: '', to: '' }), missing)
  assert.notEqual(status({ from, to: from }), missing)
  const empty = versions({ from, to, diff: diff('') })
  assert.ok(empty.root.findByType('pre').props.children)
  empty.unmount()

  for (const truncated of [false, true]) {
    const text = '+boundary = 1800'
    const view = versions({ from, to, diff: diff(text, truncated) })
    assert.equal(view.root.findByType('pre').props.children, text)
    assert.equal(view.root.findAllByType('p').some(node => !!node.props.children), truncated)
    view.unmount()
  }
  const error = versions({ from, to, error: 'not_found' })
  assert.equal(error.root.findByProps({ role: 'alert' }).props.children, 'not_found')
  error.unmount()
}

{
  // Execution list members pair by name: an inserted threshold does not shift
  // the pairs, a name repeated once per arriving input pairs by occurrence,
  // and an unlabeled member is named by its implementation number.
  const member = (runId: string, invocation: bigint, batchIndex: number, label: string | null, implementationIndex = 1) => Object.assign(summary(runId, 'score'), {
    invocation, batchActivation: 1n, batchIndex, batchSize: 4, implementationIndex, implementationLabel: label,
  })
  const left = [member('left', 1n, 1, 'threshold 10'), member('left', 2n, 2, 'threshold 14'), member('left', 3n, 3, 'threshold 10'), member('left', 4n, 4, 'threshold 14')]
  const right = [member('right', 1n, 1, 'threshold 10'), member('right', 2n, 2, 'threshold 12'), member('right', 3n, 3, 'threshold 14'),
    member('right', 4n, 4, 'threshold 10'), member('right', 5n, 5, 'threshold 12'), member('right', 6n, 6, 'threshold 14')]
  const pairs = pairMembers(left, right)
  assert.deepEqual(pairs.map(p => [p.name, p.left?.invocation, p.right?.invocation]), [
    ['threshold 10 (1)', 1n, 1n], ['threshold 14 (1)', 2n, 3n], ['threshold 10 (2)', 3n, 4n], ['threshold 14 (2)', 4n, 6n],
    ['threshold 12 (1)', undefined, 2n], ['threshold 12 (2)', undefined, 5n],
  ])
  assert.equal(memberName(member('left', 1n, 1, null, 2)), L('実装 {0}', 2))
  assert.ok(inBatch(member('left', 1n, 1, null)) && !inBatch(summary('left', 'single')))
  const one = Object.assign(member('left', 1n, 1, 'value=5'), { batchSize: 1 })
  assert.ok(inBatch(one), 'a one-member sweep is still a named batch member')

  const pages: string[] = []
  const all = await batchMembers({
    async invocations(run, card, cursor) {
      pages.push(`${run}/${card.join('/')}/${cursor ?? ''}`)
      const other = Object.assign(member(run, 9n, 1, 'earlier'), { batchActivation: 2n })
      return Object.assign(new R.InvocationPageT(), cursor ? { records: [left[3], left[2]] } : { records: [left[1], other, left[0]], nextCursor: 'next' })
    },
  }, 'left', left[3])
  assert.deepEqual(all.map(m => m.invocation), [1n, 2n, 3n, 4n], 'only the same activation, in batch order')
  assert.deepEqual(pages, ['left/score/', 'left/score/next'])

  // The view shows the last member, then the member chosen by name on both sides.
  const recorded = (run: string, m: R.InvocationSummaryT) => Object.assign(new R.RecordResultT(), {
    record: Object.assign(new R.InvocationRecordT(), { summary: m, paramsJson: `{"run":"${run}","at":${m.invocation}}`, recordIo: true }), exists: true,
  })
  const reads: string[] = []
  const rpc = {
    async invocations(run: string) { return Object.assign(new R.InvocationPageT(), { records: run === 'left' ? left : right }) },
    async record(run: string, _card: string[], invocation: bigint | null) {
      reads.push(`${run}#${invocation}`)
      return recorded(run, (run === 'left' ? left : right).find(m => m.invocation === invocation)!)
    },
  }
  const row = { card: 'score', left: { summary: left[3], record: recorded('left', left[3]) }, right: { summary: right[5], record: recorded('right', right[5]) } }
  let view!: ReturnType<typeof create>
  act(() => { view = create(<ComparedMembers rpc={rpc} left="left" right="right" row={row}>{(shown, choice) => <ComparedCardView row={shown} choice={choice} />}</ComparedMembers>) })
  // Synchronous act: the bundled async act would queue its work on MessageChannels that keep Node running.
  await setImmediate(); await setImmediate()
  const select = view.root.findByType('select')
  assert.equal(select.props.value, 'threshold 14\u00002')
  assert.ok(view.root.findAllByType('option').some(o => o.props.children === L('{0}（片方の実行のみ）', 'threshold 12 (1)')))
  assert.deepEqual(reads, [], 'the latest pair reuses the records already read')
  act(() => { select.props.onChange({ target: { value: 'threshold 10\u00001' } }) })
  await setImmediate(); await setImmediate()
  assert.deepEqual(reads.sort(), ['left#1', 'right#1'])
  const shown = view.root.findByType(ComparedCardView).props.row
  assert.equal(shown.left.summary.invocation, 1n)
  assert.equal(shown.right.summary.implementationLabel, 'threshold 10')
  view.unmount()
}

console.log('run comparison: retained Cas A table/images, params, conditions, scope, omitted values, versions, I/O, missing/failed/stopped Cards, named batch members and read-only loading passed')
