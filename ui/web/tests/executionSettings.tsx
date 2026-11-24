import { recordSettings } from '../src/model/cardFields'
import { projectFixture } from './documentFixture'
import { addCodeCard, cardIds } from './cardIds'
import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { get, num, numText, parse, str } from '../shared/json'
import { D, R } from '../src/wire/rpc'

const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')
const { CardEditorModel, CardSettingsEditor } = await import('../src/ui/cardSettings')

const session = cardIds(new ProjectSession('/p', 'k', 'p'))
session.project = parse(projectFixture({"name":"t","canvas":{"cards":[],"connections":[]}}))
session.layout = parse('{}')
session.connected = true
const card = await addCodeCard(session, { x: 0, y: 0 })
const editor = new CardEditorModel(session, card)
for (const value of recordSettings) {
  editor.setSetting('recordIO', value)
  editor.setSetting('recordLogs', value)
  assert.ok(await editor.apply())
  assert.equal(str(get(session.cardDefinition(card), 'recordIO')), value)
  assert.equal(str(get(session.cardDefinition(card), 'recordLogs')), value)
}
const bits = [0, 63]
editor.setSetting('workerCapabilities', bits.map(num))
editor.setSystemEnabled('aarch64-linux', false)
assert.deepEqual(editor.systems, ['aarch64-linux'], 'the last system cannot be disabled')
editor.setSystemEnabled('x86_64-linux', true)
editor.setSystemEnabled('aarch64-darwin', true)
editor.setSystemEnabled('aarch64-darwin', true)
editor.setEmulationFallback('x86_64-linux')
editor.setSystemEnabled('aarch64-darwin', false)
assert.ok(await editor.apply())

const def = session.cardDefinition(card)
assert.deepEqual(get(def, 'systems'), ['x86_64-linux', 'aarch64-linux'])
for (const field of ['timeoutSeconds', 'buildTimeoutSeconds']) assert.equal(numText(get(def, field)), '0')
assert.equal(Number(numText(get(def, 'sandbox', 'limits', 'cpu'))), 0)
assert.equal(numText(get(def, 'sandbox', 'limits', 'memoryMiB')), '0')
const markup = renderToStaticMarkup(<CardSettingsEditor session={session} card={card} onClose={() => {}} />)
assert.match(markup, /<option value="x86_64-linux" selected="">/)
assert.match(markup, /0 = 上限なし|0 = no limit/i)
assert.match(markup, /0, 63/)
assert.doesNotMatch(markup, /<option value="">この実行環境の既定/)
assert.match(markup, /<input(?=[^>]*name="cpu")(?=[^>]*value="0(?:\.0)?")[^>]*>/)
assert.match(markup, /<input(?=[^>]*name="memoryMiB")(?=[^>]*value="0")[^>]*>/)

const runEvents = session as unknown as { applyRunEvent(event: R.RunEventT): void }
session.runStatus = Object.assign(new R.RunStatusT(), { runId: 'current', state: D.RunState.running })
const event = (runId: string, state: D.CardState, executionState: D.RemoteExecutionState) =>
  Object.assign(new R.RunEventT(), {
    runId,
    valueType: R.RunEventValue.RunCardChanged,
    value: Object.assign(new R.RunCardChangedT(), { invocation: Object.assign(new R.InvocationSummaryT(), {
      card: Object.assign(new D.CardPathT(), { segments: [card] }), invocation: 1n, state, executionState,
    }) }),
  })
runEvents.applyRunEvent(event('current', D.CardState.running, D.RemoteExecutionState.running))
runEvents.applyRunEvent(event('previous', D.CardState.succeeded, D.RemoteExecutionState.succeeded))
assert.equal(session.remoteHistoryRevision, 2)
assert.equal(session.cardStates.get(card)?.state, D.CardState.running)
assert.equal(session.timeline.length, 1)
assert.equal(session.timeline[0].state, D.CardState.running)

console.log('Card execution settings: passed')
