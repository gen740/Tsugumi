import assert from 'node:assert/strict'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { parse } from '../src/shared/json'
import { D, R } from '../src/shared/rpc'

const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { ProjectSession } = await import('../src/model/session')
const { CardSettingsEditor } = await import('../src/ui/cardSettings')

const session = new ProjectSession('/p', 'k', 'p')
session.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
session.layout = parse('{}')
session.scheduleSave = () => {}
session.connected = true
const card = session.addCard('python', [], { x: 0, y: 0 })
const show = (nativeSystem: string, systems: string[]) => {
  session.executionServices = [Object.assign(new R.ExecutionServiceT(), { id: 'local', nativeSystem, systems })]
  return renderToStaticMarkup(<CardSettingsEditor session={session} card={card} onClose={() => {}} />)
}

const linux = show('aarch64-linux', ['aarch64-linux', 'x86_64-linux'])
assert.match(linux, /Linux \(aarch64-linux\)/)
assert.match(linux, /この実行環境の既定（aarch64-linux）/)
assert.match(linux, /<option value="x86_64-linux">/)
assert.doesNotMatch(linux, /macOS|Rosetta/)

session.updateCard(card, (def) => def.set('system', 'x86_64-linux'))
const unavailable = show('aarch64-linux', ['aarch64-linux'])
assert.match(unavailable, /<option value="x86_64-linux" disabled="" selected="">/)
assert.match(unavailable, /この実行環境では x86_64-linux を実行できません。/)

session.updateCard(card, (def) => def.set('system', undefined))
const darwin = show('aarch64-darwin', ['aarch64-darwin'])
assert.match(darwin, /macOS \(aarch64-darwin\)/)
assert.match(darwin, /この実行環境の既定（aarch64-darwin）/)
assert.doesNotMatch(darwin, /<option value="x86_64-linux"/)

session.executionServices = [
  Object.assign(new R.ExecutionServiceT(), { id: 'local', nativeSystem: 'aarch64-linux', systems: ['aarch64-linux'] }),
  Object.assign(new R.ExecutionServiceT(), { id: 'worker', nativeSystem: 'x86_64-linux', systems: ['x86_64-linux'] }),
]
session.updateCard(card, (def) => {
  def.set('executionService', 'worker')
  def.set('executionResources', parse('{"cpuMillis":1000,"memoryBytes":1073741824}'))
  def.set('sandbox', false)
})
const remote = renderToStaticMarkup(<CardSettingsEditor session={session} card={card} onClose={() => {}} />)
assert.match(remote, /<option value="worker" selected="">SSH \(worker\)/)
assert.match(remote, /この実行環境の既定（x86_64-linux）/)
assert.doesNotMatch(remote, /<option value="aarch64-linux"/)
assert.match(remote, /CPU 上限（ミリコア）/)
assert.match(remote, /メモリ上限（バイト）/)
assert.match(remote, /外部の実行サービスにはファイルアクセスの制限が必要です。/)

session.executionServices = session.executionServices.filter((entry) => entry.id === 'local')
const missingService = renderToStaticMarkup(<CardSettingsEditor session={session} card={card} onClose={() => {}} />)
assert.match(missingService, /<option value="worker" disabled="" selected="">worker/)
assert.match(missingService, /選択した実行サービスに接続できません。/)

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
