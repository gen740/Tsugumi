import { PermissionKinds } from '../src/gen/tsugumi/host/permission-kinds'
import { Settings } from '../src/gen/tsugumi/host/settings'
import { SignInStatus } from '../src/gen/tsugumi/host/sign-in-status'
import { Reply as HostReply } from '../src/gen/tsugumi/host/reply'
import { ReplyPayload as HostReplyPayload } from '../src/gen/tsugumi/host/reply-payload'
import { Payload as HostPayload } from '../src/gen/tsugumi/host/payload'
import { RecentList } from '../src/gen/tsugumi/host/recent-list'
import { DemoList } from '../src/gen/tsugumi/host/demo-list'
import { ModelList } from '../src/gen/tsugumi/host/model-list'
import { ExecutionServices } from '../src/gen/tsugumi/host/execution-services'
// Protocol boundaries are checked without sockets, including exact 64-bit
// correlation/cursors and conversion to the existing HTTP/SSE presentation DTOs.
import assert from 'node:assert/strict'
import * as fb from 'flatbuffers'
import { decodeReply, encodeRequest, hostMethod, CoreAgentError } from '../server/serviceWire'
import { Message, MessageT } from '../src/gen/tsugumi/agent/message'
import { Payload } from '../src/gen/tsugumi/agent/payload'
import { ReplyT } from '../src/gen/tsugumi/agent/reply'
import { ReplyPayload } from '../src/gen/tsugumi/agent/reply-payload'
import { RequestT } from '../src/gen/tsugumi/agent/request'
import { EventsRequestT } from '../src/gen/tsugumi/agent/events-request'
import { ConversationSendRequestT } from '../src/gen/tsugumi/agent/conversation-send-request'
import { EventsPageT } from '../src/gen/tsugumi/agent/events-page'
import { SequencedEventT } from '../src/gen/tsugumi/agent/sequenced-event'
import { EventT } from '../src/gen/tsugumi/agent/event'
import { FailureT } from '../src/gen/tsugumi/service/failure'
import { Message as HostMessage } from '../src/gen/tsugumi/host/message'

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
assert.throws(() => encodeRequest('RegisterProjectRequest', {}, 1n, false), /Unknown Agent/)
assert.throws(() => decodeReply(host, false), /identifier/)
assert.throws(() => decodeReply(frame, false), /Expected service reply/)
assert.throws(() => decodeReply(frame.subarray(0, 7), false), /Truncated/)

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
assert.ok(failure.error instanceof CoreAgentError)
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
  assert.throws(() => encodeRequest(method, fields, 2n, false), /Unknown Agent/)
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
    : SignInStatus.createSignInStatus(b, 0, b.createString('idle'), 0, 0, 0, false)
  const reply = HostReply.createReply(b, type, value)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 1n, HostPayload.Reply, reply))
  const decoded = decodeReply(b.asUint8Array(), true).value
  assert.deepEqual(type === HostReplyPayload.Settings ? decoded.values : decoded.accounts, [])
}
console.log('Settings and sign-in readers: omitted vectors are empty')

// Guide replies expose presentation fields directly; omitted vectors are empty.
const { GuideState } = await import('../src/gen/tsugumi/host/guide-state')
const { Request: HostRequest } = await import('../src/gen/tsugumi/host/request')
const { RequestPayload: HostRequestPayload } = await import('../src/gen/tsugumi/host/request-payload')
const { GuideAdvanceRequest } = await import('../src/gen/tsugumi/host/guide-advance-request')
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
const { ConversationResultT } = await import('../src/gen/tsugumi/agent/conversation-result')
const { ConversationT } = await import('../src/gen/tsugumi/agent/conversation')
const { InterruptedOperationT } = await import('../src/gen/tsugumi/agent/interrupted-operation')
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
  const { SettingsChangedT } = await import('../src/gen/tsugumi/host/settings-changed')
  const { EventT } = await import('../src/gen/tsugumi/host/event')
  const { EventPayload } = await import('../src/gen/tsugumi/host/event-payload')
  const { MessageT } = await import('../src/gen/tsugumi/host/message')
  const b = new fb.Builder()
  const message = new MessageT(0n, HostPayload.Event, new EventT(EventPayload.SettingsChanged, new SettingsChangedT('desktop', 42n)))
  HostMessage.finishMessageBuffer(b, message.pack(b))
  assert.deepEqual(decodeReply(b.asUint8Array(), true).event, { type: 'SettingsChanged', value: { profile: 'desktop', revision: 42 } })
}

// Each checkbox travels independently; no implicit Write/Run grant.
{
  const bytes = encodeRequest('AccessResolveRequest', { request_id: 'first-access', allow: true, also_allow: ['Write'] }, 5n, false)
  const message = Message.getRootAsMessage(new fb.ByteBuffer(bytes)).unpack()
  const value = (message.payload as any).value
  assert.equal(value.allow, true)
  assert.deepEqual(value.alsoAllow, [1])
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
    assert.throws(() => encodeRequest(method, fields, 7n, false), /Unknown Agent/)
  }
  const { FileFormatResult } = await import('../src/gen/tsugumi/host/file-format-result')
  const b = new fb.Builder()
  const formatted = FileFormatResult.createFileFormatResult(b, b.createString('{}\n'), 0)
  const reply = HostReply.createReply(b, HostReplyPayload.FileFormatResult, formatted)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 7n, HostPayload.Reply, reply))
  assert.deepEqual(decodeReply(b.asUint8Array(), true).value, { text: '{}\n', diagnostics: [] })
}
console.log('Host formatting and version presentation adapters: passed')

// Pure document edits and the permission catalog use typed Host requests.
{
  const { DocumentSubgraphRequestT } = await import('../src/gen/tsugumi/host/document-subgraph-request')
  const { PermissionKindsRequestT } = await import('../src/gen/tsugumi/host/permission-kinds-request')
  const bytes = encodeRequest('DocumentSubgraphRequest', { project_json: '{"canvas":{}}', cards: ['a'], id: 'sub' }, high, true)
  const message = HostMessage.getRootAsMessage(new fb.ByteBuffer(bytes)).unpack()
  const value = (message.payload as any).value as InstanceType<typeof DocumentSubgraphRequestT>
  assert.ok(value instanceof DocumentSubgraphRequestT)
  assert.equal(value.projectJson, '{"canvas":{}}')
  assert.deepEqual(value.cards, ['a'])
  assert.equal(value.id, 'sub')
  const catalog = HostMessage.getRootAsMessage(new fb.ByteBuffer(encodeRequest('PermissionKindsRequest', { language: 'en' }, 9n, true))).unpack()
  assert.ok((catalog.payload as any).value instanceof PermissionKindsRequestT)
  assert.equal((catalog.payload as any).value.language, 'en')
}

{
  const { DocumentMergeRequestT } = await import('../src/gen/tsugumi/host/document-merge-request')
  const { DocumentMerge } = await import('../src/gen/tsugumi/host/document-merge')
  const frame = encodeRequest('DocumentMergeRequest', { base_json: '{}', ours_json: '{"n":2}', theirs_json: '{}' }, 1n, true)
  const request = HostMessage.getRootAsMessage(new fb.ByteBuffer(frame)).unpack().payload as any
  assert.ok(request.value instanceof DocumentMergeRequestT)
  assert.equal(request.value.oursJson, '{"n":2}')
  const b = new fb.Builder()
  const json = b.createString('{}')
  const result = DocumentMerge.createDocumentMerge(b, json, 0)
  const reply = HostReply.createReply(b, HostReplyPayload.DocumentMerge, result)
  const message = HostMessage.createMessage(b, 1n, HostPayload.Reply, reply)
  b.finish(message, 'TSH1')
  assert.deepEqual(decodeReply(b.asUint8Array(), true).value.conflicts, [])
}

// Every new operation uses the Host envelope, including omitted file vectors.
{
  const { DocumentCopyRequestT } = await import('../src/gen/tsugumi/host/document-copy-request')
  const { DocumentCopy } = await import('../src/gen/tsugumi/host/document-copy')
  const copy = HostMessage.getRootAsMessage(new fb.ByteBuffer(encodeRequest('DocumentCopyRequest', { state_json: '{}', from: 'externals/a/', to: 'externals/b/' }, high, true))).unpack().payload as any
  assert.ok(copy.value instanceof DocumentCopyRequestT)
  assert.deepEqual(copy.value.paths, [])
  assert.equal(copy.value.to, 'externals/b/')
  const b = new fb.Builder(), json = b.createString('{}')
  const value = DocumentCopy.createDocumentCopy(b, json, 0)
  const reply = HostReply.createReply(b, HostReplyPayload.DocumentCopy, value)
  const root = HostMessage.createMessage(b, 1n, HostPayload.Reply, reply)
  b.finish(root, 'TSH1')
  assert.deepEqual(decodeReply(b.asUint8Array(), true).value.files, [])
  const connect = HostMessage.getRootAsMessage(new fb.ByteBuffer(encodeRequest('DocumentConnectRequest', { state_json: '{}', from_card: 'a', from_port: '+', to_card: 'b', to_port: 'in' }, 2n, true))).unpack().payload as any
  assert.equal(connect.value.fromPort, '+')
  assert.equal(connect.value.forkId, null)
  const template = HostMessage.getRootAsMessage(new fb.ByteBuffer(encodeRequest('DocumentTemplateRequest', { name: 'python', id: 'new-card' }, 3n, true))).unpack().payload as any
  assert.equal(template.value.name, 'python')
  assert.equal(template.value.id, 'new-card')
}

// Flatc goldens are shared with Swift and C++ so union renumbering cannot
// silently send a moved Host method to the external Agent listener.
const fs = await import('node:fs')
const path = await import('node:path')
const { BFBS } = await import('../src/shared/bfbs')
const { ToolCatalog } = await import('../server/agent')
const serviceVectors = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'tests/fixtures/service-contract.json'), 'utf8'))
for (const c of serviceVectors.requests) {
  assert.equal(hostMethod(c.method), c.host, c.method)
  const native = (bytes: Uint8Array) => c.host
    ? HostMessage.getRootAsMessage(new fb.ByteBuffer(bytes)).unpack()
    : Message.getRootAsMessage(new fb.ByteBuffer(bytes)).unpack()
  const golden = native(Buffer.from(c.hex, 'hex'))
  assert.deepEqual(native(encodeRequest(c.method, c.fields, 7n, c.host)), golden, c.method)
  assert.throws(() => encodeRequest(c.method, c.fields, 7n, !c.host), /Unknown/)
}
for (const c of serviceVectors.replies) {
  const decoded = decodeReply(Buffer.from(c.hex, 'hex'), c.host).value
  if (Array.isArray(c.expect)) assert.deepEqual(decoded, c.expect)
  else for (const [key, value] of Object.entries(c.expect)) assert.deepEqual(decoded[key], value)
}
const agentSchema = new BFBS(new Uint8Array(fs.readFileSync(path.join(process.argv[2], 'web/src/gen/agent.bfbs'))))
const agentRequests = agentSchema.enums.find(e => e.name === 'Tsugumi.Agent.RequestPayload')!.values
  .filter(v => v.value && v.unionType && v.name !== 'HelloRequest').map(v => v.name)
assert.deepEqual(agentRequests, serviceVectors.agentRequests)
const externalCatalog = new ToolCatalog(agentSchema, agentRequests, 'Tsugumi.Agent')
for (const method of ['demo_list', 'demo_read', 'version_note']) assert.equal(externalCatalog.tool(method), undefined)
for (const method of ['tool_call', 'access', 'conversation_summarize']) assert.ok(externalCatalog.tool(method))
console.log('Separate Host/Agent contracts: shared goldens, routes and external MCP catalog passed')

// Reset confirmations and the helper's actual file report survive the host adapter.
{
  const { HostRuntimeT } = await import('../src/gen/tsugumi/host/host-runtime')
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

// Heartbeat values survive the JSON/binary boundary without integer coercion.
for (const [interval, missed] of [[15, 0], [2, 3], [0xffffffff, 0xffffffff]]) {
  const bytes = encodeRequest('ExecutionServicePutRequest', {
    id: 'compute', name: 'Compute', endpoint: 'tls://worker:8765', workerPublicKey: 'ed25519:test',
    heartbeatIntervalSeconds: interval, missedHeartbeats: missed,
  }, 2n, true)
  const request = HostMessage.getRootAsMessage(new fb.ByteBuffer(bytes)).unpack().payload as any
  assert.equal(request.value.heartbeatIntervalSeconds, interval)
  assert.equal(request.value.missedHeartbeats, missed)
}
for (const [key, values] of [
  ['heartbeatIntervalSeconds', [0, -1, 1.5, 0x100000000, NaN, '15', null]],
  ['missedHeartbeats', [-1, 1.5, 0x100000000, NaN, '0', null]],
] as const) {
  for (const value of values) assert.throws(() => encodeRequest('ExecutionServicePutRequest', {
    id: 'compute', name: 'Compute', endpoint: 'tls://worker:8765', workerPublicKey: 'ed25519:test', [key]: value,
  }, 2n, true), /Invalid heartbeat setting/)
}

const { ExecutionServiceT } = await import('../src/gen/tsugumi/host/execution-service')
const { ExecutionCapabilitiesT } = await import('../src/gen/tsugumi/host/execution-capabilities')
const { ExecutionServicesT } = await import('../src/gen/tsugumi/host/execution-services')
{
  const b = new fb.Builder()
  const entry = new ExecutionServiceT('compute', 'Compute', 'tls://worker:8765', 'ed25519:test', new ExecutionCapabilitiesT('server', 'x86_64-linux'), 2, 3)
  const services = new ExecutionServicesT([entry], 'ed25519:host').pack(b)
  const reply = HostReply.createReply(b, HostReplyPayload.ExecutionServices, services)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 1n, HostPayload.Reply, reply))
  const state = decodeReply(b.asUint8Array(), true).value
  assert.equal(state.services[0].heartbeatIntervalSeconds, 2)
  assert.equal(state.services[0].missedHeartbeats, 3)
}
