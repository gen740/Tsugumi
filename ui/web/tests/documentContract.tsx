import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { get, numText, str } from '../shared/json'
import { toggleWorkerBit, validWorkerBits } from '../shared/workerBits'
import { implementedCard, recordSettings } from '../src/model/cardFields'
import { cardNotes, parseReadme, printReadme, splitCardNotes } from '../src/model/documents'
import { cardStem, nextInputName, validName } from '../src/model/names'

for (const description of ['description:', 'description: ']) {
  const readme = `---\n${description}\ntitle: 集計: Results\n---\n\nSteps`
  assert.deepEqual(parseReadme(readme), { title: '集計: Results', description: '', body: 'Steps' })
  const notes = cardNotes(readme, null)
  assert.equal(splitCardNotes(notes).readme, readme, 'the selected value keeps the original bytes')
}
assert.equal(printReadme({ title: '集計: Results', description: '', body: '' }), '---\ntitle: 集計: Results\ndescription: \n---\n')
assert.equal(parseReadme('---\ntitle: Missing description\n---\n'), undefined)
assert.equal(parseReadme('---\ntitle: Results\ndescription:missing space\n---\n'), undefined)
assert.equal(parseReadme('---\ntitle: Results\ndescription: \nunknown: value\n---\n'), undefined)

const executable = ['type', 'symbol', 'inputs', 'maxRuns', 'reuseWorker', 'timeoutSeconds',
  'buildTimeoutSeconds', 'recordLogs', 'recordIO', 'sandbox', 'grants', 'system', 'workerCapabilities'].sort()
for (const kind of ['code', 'input']) {
  const def = implementedCard(kind, 'star', 'aarch64-linux')
  assert.deepEqual(def.keys().sort(), executable)
  assert.equal(str(get(def, 'recordIO')), 'project')
  assert.equal(str(get(def, 'recordLogs')), 'project')
  assert.equal(numText(get(def, 'timeoutSeconds')), '0')
  assert.equal(numText(get(def, 'buildTimeoutSeconds')), '0')
  assert.equal(numText(get(def, 'sandbox', 'limits', 'cpu')), '0')
  assert.equal(numText(get(def, 'sandbox', 'limits', 'memoryMiB')), '0')
  assert.deepEqual(get(def, 'workerCapabilities'), [])
}
for (const [kind, keys] of Object.entries({
  merge: ['type', 'symbol', 'inputs', 'maxRuns', 'recordIO'],
  collect: ['type', 'symbol', 'maxRuns', 'recordIO'],
  draft: ['type', 'symbol'],
  trigger: ['type', 'symbol'],
  subgraph: ['type', 'symbol'],
})) assert.deepEqual(implementedCard(kind, 'star', '').keys().sort(), keys.sort())
assert.deepEqual(recordSettings, ['project', 'on', 'off'])

const bits = toggleWorkerBit(toggleWorkerBit([], 63), 0)
assert.deepEqual(bits, [0, 63])
assert.deepEqual(toggleWorkerBit(toggleWorkerBit(bits, 0), 63), [])
for (const invalid of [[0, 0], [-1], [64], [1.5]]) assert.equal(validWorkerBits(invalid), false)
assert.ok(validWorkerBits(bits))

for (const valid of ['a', 'example_2', 'a'.repeat(64)]) assert.ok(validName(valid))
for (const invalid of ['', 'Example', '2nd', '_x', 'card-name', 'a\n', 'a'.repeat(65), '集計']) assert.ok(!validName(invalid))
assert.equal(nextInputName(['in1', 'in3']), 'in2')
assert.equal(cardStem('42 — Area Ratio'), 'area_ratio')
for (const title of ['集計', '2nd', 'A'.repeat(70), 'other_id']) assert.ok(validName(cardStem(title)))

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { WorkerBitsEditor } = await import('../src/ui/workerBits')
const editor = renderToStaticMarkup(<WorkerBitsEditor bits={bits} onChange={() => {}} />)
assert.match(editor, /0, 63/)
assert.match(editor, /<option value="63">✓ 63<\/option>/)
assert.doesNotMatch(editor, /<option value="64"/)
console.log('Explicit project file contract: passed')
