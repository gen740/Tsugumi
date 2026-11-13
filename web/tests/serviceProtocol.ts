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
import { EventsPageT } from '../src/gen/tsugumi/agent/events-page'
import { SequencedEventT } from '../src/gen/tsugumi/agent/sequenced-event'
import { EventT } from '../src/gen/tsugumi/agent/event'
import { FailureT } from '../src/gen/tsugumi/service/failure'
import { Message as HostMessage } from '../src/gen/tsugumi/host/message'

const high = (1n << 64n) - 2n
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
