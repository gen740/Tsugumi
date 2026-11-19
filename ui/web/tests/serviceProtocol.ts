import { Access } from '../shared/gen/tsugumi/service/access'
import { ProjectTrust } from '../shared/gen/tsugumi/host/project-trust'
import { PermissionKinds } from '../shared/gen/tsugumi/host/permission-kinds'
import { Settings } from '../shared/gen/tsugumi/host/settings'
import { SignInStatus } from '../shared/gen/tsugumi/host/sign-in-status'
import { SignInChangedT } from '../shared/gen/tsugumi/host/sign-in-changed'
import { EventT as HostEventT } from '../shared/gen/tsugumi/host/event'
import { EventPayload as HostEventPayload } from '../shared/gen/tsugumi/host/event-payload'
import { Reply as HostReply } from '../shared/gen/tsugumi/host/reply'
import { ReplyPayload as HostReplyPayload } from '../shared/gen/tsugumi/host/reply-payload'
import { Payload as HostPayload } from '../shared/gen/tsugumi/host/payload'
import { RecentList } from '../shared/gen/tsugumi/host/recent-list'
import { DemoList } from '../shared/gen/tsugumi/host/demo-list'
import { ModelList } from '../shared/gen/tsugumi/host/model-list'
import { ExecutionServices } from '../shared/gen/tsugumi/host/execution-services'
// Protocol boundaries are checked without sockets, including exact 64-bit
// correlation/cursors and conversion to the existing HTTP/SSE presentation DTOs.
import assert from 'node:assert/strict'
import * as fb from 'flatbuffers'
import { decodeReply, encodeRequest, hostMethod, CoreHostError } from '../server/serviceWire'
import { Message, MessageT } from '../shared/gen/tsugumi/agent/message'
import { Payload } from '../shared/gen/tsugumi/agent/payload'
import { ReplyT } from '../shared/gen/tsugumi/agent/reply'
import { ReplyPayload } from '../shared/gen/tsugumi/agent/reply-payload'
import { RequestT } from '../shared/gen/tsugumi/agent/request'
import { EventsRequestT } from '../shared/gen/tsugumi/agent/events-request'
import { ConversationSendRequestT } from '../shared/gen/tsugumi/agent/conversation-send-request'
import { EventsPageT } from '../shared/gen/tsugumi/agent/events-page'
import { SequencedEventT } from '../shared/gen/tsugumi/agent/sequenced-event'
import { EventT } from '../shared/gen/tsugumi/agent/event'
import { FailureT } from '../shared/gen/tsugumi/service/failure'
import { Message as HostMessage, MessageT as HostMessageT } from '../shared/gen/tsugumi/host/message'
import { hostProjectErrorText } from '../server/hostProjects'
import { setLanguage } from '../server/i18n'

const high = (1n << 64n) - 2n
const sent = Message.getRootAsMessage(new fb.ByteBuffer(encodeRequest('ConversationSendRequest', {
  conversation_id: 'c', turn_id: 't', text: 'Model instructions', display: 'hello instead', selected_cards: [], selected_displays: [],
}, 1n, false))).unpack()
assert.equal(((sent.payload as RequestT).value as ConversationSendRequestT).display, 'hello instead')
const frame = encodeRequest('EventsRequest', { conversation_id: '日本語', after_seq: high }, high, false)
const request = Message.getRootAsMessage(new fb.ByteBuffer(frame)).unpack()
assert.equal(request.requestId, high)
assert.equal(((request.payload as RequestT).value as EventsRequestT).afterSeq, high)
assert.equal(((request.payload as RequestT).value as EventsRequestT).conversationId, '日本語')
const host = encodeRequest('HostRecentListRequest', {}, 1n, true)
assert.equal(HostMessage.bufferHasIdentifier(new fb.ByteBuffer(host)), true)
assert.equal(Message.bufferHasIdentifier(new fb.ByteBuffer(host)), false)
assert.equal(hostMethod('RegisterProjectRequest'), true)
assert.throws(() => encodeRequest('RegisterProjectRequest', {}, 1n, false))

function reply(type: ReplyPayload, value: ReplyT['value']) {
  const b = new fb.Builder()
  Message.finishMessageBuffer(b, new MessageT(high, Payload.Reply, new ReplyT(type, value)).pack(b))
  return b.asUint8Array()
}
const event = new EventT('toolStarted', null, null, '{"x":1}', 'call', 'guide_show')
const decoded = decodeReply(reply(ReplyPayload.EventsPage, new EventsPageT([
  new SequencedEventT(high, event),
], high, true)), false)
assert.equal(decoded.id, high)
assert.equal(decoded.value.next_seq, high)
assert.equal(decoded.value.events[0].seq, high)
assert.equal(decoded.value.events[0].event.input, '{"x":1}')
const failure = decodeReply(reply(ReplyPayload.Failure, new FailureT('grant_required', 'Ask first', 'permission-1')), false)
assert.ok(failure.error instanceof CoreHostError)
assert.equal(failure.error.requestId, 'permission-1')
console.log('Agent/Host binary protocol: exact uint64, separation, typed events and failures passed')

// Execution settings send management requests only to the Host contract.
for (const [method, fields] of [
  ['ExecutionServicesRequest', {}],
  ['ExecutionServicePutRequest', { id: 'compute', name: 'Compute', endpoint: 'unix:/tmp/worker', workerPublicKey: 'ed25519:test' }],
  ['ExecutionServiceRemoveRequest', { id: 'compute' }],
  ['ExecutionProjectAuthorizeRequest', { project_id: 'project', path: '/project', control_dir: '/control', guest: true }],
  ['ExecutionProjectRevokeRequest', { project_id: 'project' }],
] as const) {
  assert.equal(hostMethod(method), true)
  const bytes = encodeRequest(method, fields, 2n, true)
  const request = HostMessage.getRootAsMessage(new fb.ByteBuffer(bytes)).unpack()
  assert.equal(request.requestId, 2n)
  assert.throws(() => encodeRequest(method, fields, 2n, false))
}
console.log('Execution service settings: Host RPC adapters passed')

// C++ object packing omits optional empty vectors. Every Host list reader
// must return an empty list even when the vector offset is absent on the wire.
for (const type of [HostReplyPayload.RecentList, HostReplyPayload.DemoList, HostReplyPayload.ModelList, HostReplyPayload.ExecutionServices, HostReplyPayload.PermissionKinds]) {
  const b = new fb.Builder()
  let value: fb.Offset
  switch (type) {
    case HostReplyPayload.RecentList: value = RecentList.createRecentList(b, 0); break
    case HostReplyPayload.DemoList: value = DemoList.createDemoList(b, 0); break
    case HostReplyPayload.ModelList: value = ModelList.createModelList(b, 0); break
    case HostReplyPayload.PermissionKinds: value = PermissionKinds.createPermissionKinds(b, 0); break
    default: value = ExecutionServices.createExecutionServices(b, 0, b.createString('ed25519:fixture'))
  }
  const reply = HostReply.createReply(b, type, value)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 1n, HostPayload.Reply, reply))
  const decoded = decodeReply(b.asUint8Array(), true).value
  assert.deepEqual(type === HostReplyPayload.ExecutionServices ? decoded.services : type === HostReplyPayload.PermissionKinds ? decoded.kinds : decoded, [])
}
console.log('Host list readers: omitted vectors are empty')

for (const type of [HostReplyPayload.Settings, HostReplyPayload.SignInStatus]) {
  const b = new fb.Builder()
  const value = type === HostReplyPayload.Settings ? Settings.createSettings(b, 0n, 0)
    : SignInStatus.createSignInStatus(b, 0, b.createString('idle'), 0, 0, 0, false, 0, false)
  const reply = HostReply.createReply(b, type, value)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 1n, HostPayload.Reply, reply))
  const decoded = decodeReply(b.asUint8Array(), true).value
  assert.deepEqual(type === HostReplyPayload.Settings ? decoded.values : decoded.accounts, [])
}
console.log('Settings and sign-in readers: omitted vectors are empty')

{
  const b = new fb.Builder()
  const attempt = b.createString('attempt-a')
  const client = b.createString('account-a')
  const status = SignInStatus.createSignInStatus(b, attempt, b.createString('complete'), 0, 0, 0, false, client, true)
  const reply = HostReply.createReply(b, HostReplyPayload.SignInStatus, status)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 1n, HostPayload.Reply, reply))
  const decoded = decodeReply(b.asUint8Array(), true).value
  assert.equal(decoded.attempt_id, 'attempt-a')
  assert.equal(decoded.client_id, 'account-a')
  assert.equal(decoded.planEnabled, true)
}
{
  const b = new fb.Builder()
  const change = new HostEventT(HostEventPayload.SignInChanged, new SignInChangedT('web:fixture'))
  HostMessage.finishMessageBuffer(b, new HostMessageT(0n, HostPayload.Event, change).pack(b))
  assert.deepEqual(decodeReply(b.asUint8Array(), true).event, { type: 'SignInChanged', value: { profile: 'web:fixture' } })
}

// Guide replies expose presentation fields directly; omitted vectors are empty.
const { GuideState } = await import('../shared/gen/tsugumi/host/guide-state')
const { Request: HostRequest } = await import('../shared/gen/tsugumi/host/request')
const { RequestPayload: HostRequestPayload } = await import('../shared/gen/tsugumi/host/request-payload')
const { GuideAdvanceRequest } = await import('../shared/gen/tsugumi/host/guide-advance-request')
{
  const b = new fb.Builder()
  const session = b.createString('web:host')
  GuideState.startGuideState(b)
  GuideState.addSessionId(b, session)
  const value = GuideState.endGuideState(b)
  const reply = HostReply.createReply(b, HostReplyPayload.GuideState, value)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 1n, HostPayload.Reply, reply))
  const decoded = decodeReply(b.asUint8Array(), true).value
  assert.deepEqual(decoded.choices, [])
  assert.deepEqual(decoded.actions, [])
  assert.equal(decoded.sessionId, 'web:host')
  assert.equal(decoded.value, undefined)
  const bytes = encodeRequest('GuideAdvanceRequest', { session_id: 'web:host', language: 'en',
    event: { kind: 'message', text: 'Next', expected_step: 'report/start', project: '/copy' } }, 2n, true)
  const req = HostMessage.getRootAsMessage(new fb.ByteBuffer(bytes)).payload(new HostRequest())!
  assert.equal(req.valueType(), HostRequestPayload.GuideAdvanceRequest)
  const guide = req.value(new GuideAdvanceRequest())!
  assert.equal(guide.event()?.expectedStep(), 'report/start')
  assert.equal(guide.event()?.scripted(), true)
  assert.equal(hostMethod('GuideGetRequest'), true)
}
console.log('Guide Host adapter: events, direct presentation and omitted vectors passed')

// The interrupted operation survives typed packing and HTTP adaptation.
const { ConversationResultT } = await import('../shared/gen/tsugumi/agent/conversation-result')
const { ConversationT } = await import('../shared/gen/tsugumi/agent/conversation')
const { InterruptedOperationT } = await import('../shared/gen/tsugumi/agent/interrupted-operation')
const interrupted = new ConversationResultT()
interrupted.conversation = new ConversationT()
interrupted.conversation.id = 'interrupted'
interrupted.resumable = true
interrupted.interruptedOperation = new InterruptedOperationT('tool', 'call-7', 'project_save', '{"changes":[]}')
const interruptedView = decodeReply(reply(ReplyPayload.ConversationResult, interrupted), false).value
assert.equal(interruptedView.resumable, true)
assert.deepEqual(interruptedView.interruptedOperation, { kind: 'tool', id: 'call-7', name: 'project_save', arguments: '{"changes":[]}' })

// Settings changes travel on the existing Host event stream.
{
  const { SettingsChangedT } = await import('../shared/gen/tsugumi/host/settings-changed')
  const { EventT } = await import('../shared/gen/tsugumi/host/event')
  const { EventPayload } = await import('../shared/gen/tsugumi/host/event-payload')
  const { MessageT } = await import('../shared/gen/tsugumi/host/message')
  const b = new fb.Builder()
  const message = new MessageT(0n, HostPayload.Event, new EventT(EventPayload.SettingsChanged, new SettingsChangedT('desktop', 42n)))
  HostMessage.finishMessageBuffer(b, message.pack(b))
  assert.deepEqual(decodeReply(b.asUint8Array(), true).event, { type: 'SettingsChanged', value: { profile: 'desktop', revision: 42 } })
}

// Each checkbox travels independently; no implicit Write/Run grant.
{
  const bytes = encodeRequest('AccessResolveRequest', { request_id: 'first-access', allow: true, selected: [Access.Write] }, 5n, false)
  const message = Message.getRootAsMessage(new fb.ByteBuffer(bytes)).unpack()
  const value = (message.payload as any).value
  assert.equal(value.allow, true)
  assert.deepEqual(value.selected, [Access.Write])
}

// Formatting and version presentation are Host operations, independently of the core PATH/language.
{
  for (const [method, fields] of [
    ['FileFormatRequest', { path: 'x.json', text: '{"a":1}' }],
    ['HostVersionGraphRequest', { project: 'p', language: 'en' }],
    ['HostVersionDiffRequest', { project: 'p', commit: 'next', from_commit: 'before' }],
  ] as const) {
    assert.equal(hostMethod(method), true)
    const frame = encodeRequest(method, fields, 7n, true)
    const request = HostMessage.getRootAsMessage(new fb.ByteBuffer(frame)).unpack()
    const value = (request.payload as any).value
    for (const [key, field] of Object.entries(fields)) assert.equal(value[key === 'from_commit' ? 'fromCommit' : key], field)
    assert.throws(() => encodeRequest(method, fields, 7n, false))
  }
  const { FileFormatResult } = await import('../shared/gen/tsugumi/host/file-format-result')
  const b = new fb.Builder()
  const formatted = FileFormatResult.createFileFormatResult(b, b.createString('{}\n'), 0)
  const reply = HostReply.createReply(b, HostReplyPayload.FileFormatResult, formatted)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 7n, HostPayload.Reply, reply))
  assert.deepEqual(decodeReply(b.asUint8Array(), true).value, { text: '{}\n', diagnostics: [] })
}
console.log('Host formatting and version presentation adapters: passed')

// Flatc goldens are shared with Swift and C++ so union renumbering cannot
// silently send a moved Host method to the external Agent listener.
const fs = await import('node:fs')
const path = await import('node:path')
const { BFBS } = await import('../shared/bfbs')
const { ToolCatalog } = await import('../server/agent')
const serviceVectors = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'tests/fixtures/service-contract.json'), 'utf8'))
for (const c of serviceVectors.requests) {
  assert.equal(hostMethod(c.method), c.host, c.method)
  const native = (bytes: Uint8Array) => c.host
    ? HostMessage.getRootAsMessage(new fb.ByteBuffer(bytes)).unpack()
    : Message.getRootAsMessage(new fb.ByteBuffer(bytes)).unpack()
  const golden = native(Buffer.from(c.hex, 'hex'))
  assert.deepEqual(native(encodeRequest(c.method, c.fields, 7n, c.host)), golden, c.method)
  assert.throws(() => encodeRequest(c.method, c.fields, 7n, !c.host))
}
for (const c of serviceVectors.replies) {
  const decoded = decodeReply(Buffer.from(c.hex, 'hex'), c.host).value
  if (Array.isArray(c.expect)) assert.deepEqual(decoded, c.expect)
  else for (const [key, value] of Object.entries(c.expect)) assert.deepEqual(decoded[key], value)
}
const agentSchema = new BFBS(new Uint8Array(fs.readFileSync(path.join(process.argv[2], 'ui/web/shared/gen/agent.bfbs'))))
const agentRequests = agentSchema.enums.find(e => e.name === 'Tsugumi.Agent.RequestPayload')!.values
  .filter(v => v.value && v.unionType && !['HelloRequest', 'ToolCallCancelRequest', 'CallIdRequest'].includes(v.name)).map(v => v.name)
assert.deepEqual(new Set(agentRequests), new Set(serviceVectors.agentRequests))
const externalCatalog = new ToolCatalog(agentSchema, agentRequests)
for (const method of ['demo_list', 'demo_read', 'version_note', 'call_id']) assert.equal(externalCatalog.tools.find(tool => tool.name === method), undefined)
for (const method of ['tool_call', 'access', 'conversation_summarize']) assert.ok(externalCatalog.tools.find(tool => tool.name === method))
console.log('Separate Host/Agent contracts: shared goldens, routes and external MCP catalog passed')

// Reset confirmations and the helper's actual file report survive the host adapter.
{
  const { HostRuntimeT } = await import('../shared/gen/tsugumi/host/host-runtime')
  const runtime = new HostRuntimeT()
  runtime.vmState = 'stopped'
  runtime.resetConfirmationJa = '履歴を保持'
  runtime.resetConfirmationEn = 'Keep history'
  runtime.resetKeptFiles = ['history.img']
  runtime.resetDiscardedFiles = ['store.img', '.lock']
  const b = new fb.Builder()
  const offset = runtime.pack(b)
  const reply = HostReply.createReply(b, HostReplyPayload.HostRuntime, offset)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 1n, HostPayload.Reply, reply))
  const decoded = decodeReply(b.asUint8Array(), true).value
  assert.equal(decoded.vmState, 'stopped')
  assert.equal(decoded.resetConfirmationJa, '履歴を保持')
  assert.equal(decoded.resetConfirmationEn, 'Keep history')
  assert.deepEqual(decoded.resetKeptFiles, ['history.img'])
  assert.deepEqual(decoded.resetDiscardedFiles, ['store.img', '.lock'])
}

const { ExecutionServiceT } = await import('../shared/gen/tsugumi/host/execution-service')
const { ExecutionCapabilitiesT } = await import('../shared/gen/tsugumi/host/execution-capabilities')
const { ExecutionServicesT } = await import('../shared/gen/tsugumi/host/execution-services')
{
  const b = new fb.Builder()
  const entry = new ExecutionServiceT('compute', 'Compute', 'tls://worker:8765', 'ed25519:test', new ExecutionCapabilitiesT('server', 'x86_64-linux'))
  const services = new ExecutionServicesT([entry], 'ed25519:host').pack(b)
  const reply = HostReply.createReply(b, HostReplyPayload.ExecutionServices, services)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 1n, HostPayload.Reply, reply))
  const state = decodeReply(b.asUint8Array(), true).value
  assert.equal(state.services[0].id, 'compute')
}

// Match the native adapter: inherited trust identifies the host registration.
{
  const b = new fb.Builder()
  const folder = b.createString('/trials')
  const value = ProjectTrust.createProjectTrust(b, 1, folder)
  const reply = HostReply.createReply(b, HostReplyPayload.ProjectTrust, value)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 1n, HostPayload.Reply, reply))
  assert.deepEqual(decodeReply(b.asUint8Array(), true).value, { decision: 1, trusted_folder: '/trials' })
}

// A project's conversation failure keeps a recovery action in both languages.
for (const language of ['ja', 'en'] as const) {
  setLanguage(language)
  const text = hostProjectErrorText('conversation_identity_mismatch')
  assert.ok(text.trim())
  assert.notEqual(text, hostProjectErrorText('stale_project'))
  assert.notEqual(text, hostProjectErrorText('unknown'))
  assert.notEqual(hostProjectErrorText('stale_project'), hostProjectErrorText('unknown'))
}
setLanguage('ja')
