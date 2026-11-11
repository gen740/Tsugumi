import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { EvidenceSource, evidenceExists, parseEvidence } from '../src/model/evidence'
import { Markdown } from '../src/ui/markdown'

const cases = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'tests/fixtures/evidence-links.json'), 'utf8'))
for (const c of cases) {
  const target = parseEvidence(c.href)
  assert.equal(target?.kind, c.kind, c.href)
  if (c.kind) for (const field of ['card', 'invocation', 'offset', 'artifact', 'member']) {
    const value = target![field as keyof typeof target]
    assert.deepEqual(typeof value === 'bigint' ? String(value) : value, c[field], `${c.href} ${field}`)
  }
}
const run = '12345678-1234-1234-1234-123456789abc'
const calls: string[] = []
const source: EvidenceSource = {
  cardExists: card => card.join('/') === 'spectrum',
  runExists: async id => { calls.push(`run:${id}`); return id === run },
  recordExists: async (_, card, invocation) => card.join('/') === 'spectrum' && (invocation === undefined || invocation === 2n),
  logExists: async target => target.offset === 128n && target.stream === 'stderr',
  artifactExists: async target => target.artifact === 'saved' && target.member === 'plot.png',
}
assert.equal(await evidenceExists(parseEvidence('card:spectrum')!, source), true)
assert.equal(await evidenceExists(parseEvidence('card:missing')!, source), false)
assert.equal(await evidenceExists(parseEvidence(`run:${run}/spectrum#2`)!, source), true)
assert.equal(await evidenceExists(parseEvidence(`run:${run}/spectrum#3`)!, source), false)
assert.equal(await evidenceExists(parseEvidence(`run:${run}/deleted#2`)!, source), false)
assert.equal(await evidenceExists(parseEvidence(`log:${run}/spectrum?invocation=2&stream=stderr&offset=128`)!, source), true)
assert.equal(await evidenceExists(parseEvidence(`log:${run}/spectrum?offset=129`)!, source), false)
assert.equal(await evidenceExists(parseEvidence(`artifact:${run}?id=saved&path=plot.png`)!, source), true)
assert.equal(await evidenceExists(parseEvidence(`artifact:${run}?id=missing`)!, source), false)
assert.equal(await evidenceExists(parseEvidence(`run:${run}`)!, { ...source, runExists: async () => { throw Error('offline') } }), false)
assert.ok(calls.every(value => value === `run:${run}`))
let touchedOtherRecord = false
assert.equal(await evidenceExists(parseEvidence('run:00000000-0000-0000-0000-000000000000/spectrum#2')!, {
  ...source, recordExists: async () => { touchedOtherRecord = true; return true },
}), false)
assert.equal(touchedOtherRecord, false, 'A run outside the current project is rejected before any record lookup')
const html = renderToStaticMarkup(<Markdown text="[Card](card:spectrum) [missing](card:missing) [web](https://example.com)" renderLink={(href, label) => href === 'card:spectrum' ? <button>{label}</button> : href.startsWith('card:') ? <span aria-disabled="true">{label}</span> : undefined} />)
assert.match(html, /<button>Card<\/button>/)
assert.match(html, /aria-disabled="true">missing/)
assert.match(html, /href="https:\/\/example.com"/)
assert.doesNotMatch(html, /href="card:/)
console.log('evidence links: passed')
