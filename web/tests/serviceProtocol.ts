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
for (const type of [HostReplyPayload.RecentList, HostReplyPayload.DemoList, HostReplyPayload.ModelList, HostReplyPayload.ExecutionServices]) {
  const b = new fb.Builder()
  let value: fb.Offset
  switch (type) {
    case HostReplyPayload.RecentList: value = RecentList.createRecentList(b, 0); break
    case HostReplyPayload.DemoList: value = DemoList.createDemoList(b, 0); break
    case HostReplyPayload.ModelList: value = ModelList.createModelList(b, 0); break
    default: value = ExecutionServices.createExecutionServices(b, 0, b.createString('ed25519:fixture'))
  }
  const reply = HostReply.createReply(b, type, value)
  HostMessage.finishMessageBuffer(b, HostMessage.createMessage(b, 1n, HostPayload.Reply, reply))
  const decoded = decodeReply(b.asUint8Array(), true).value
  assert.deepEqual(type === HostReplyPayload.ExecutionServices ? decoded.services : decoded, [])
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
