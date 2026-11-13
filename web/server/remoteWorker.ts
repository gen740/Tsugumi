import * as flatbuffers from 'flatbuffers'
import { Readable, Writable } from 'node:stream'
import { Message, MessageT } from '../src/gen/tsugumi/remote-worker/message'
import { Payload } from '../src/gen/tsugumi/remote-worker/payload'
import { ReplyT } from '../src/gen/tsugumi/remote-worker/reply'
import { ReplyBody } from '../src/gen/tsugumi/remote-worker/reply-body'
import { FailureT } from '../src/gen/tsugumi/remote-worker/failure'
import { ErrorCode } from '../src/gen/tsugumi/remote-worker/error-code'

export { MessageT, Payload, ReplyT, ReplyBody, ErrorCode }
export { RequestT } from '../src/gen/tsugumi/remote-worker/request'
export { RequestBody } from '../src/gen/tsugumi/remote-worker/request-body'
export { ProbeRequestT } from '../src/gen/tsugumi/remote-worker/probe-request'
export { CapabilitiesT } from '../src/gen/tsugumi/remote-worker/capabilities'
export { CatalogT } from '../src/gen/tsugumi/remote-worker/catalog'
export { TargetT } from '../src/gen/tsugumi/remote-worker/target'
export const contract = 3
export const maxFrame = 2 << 20

export function encode(message: MessageT): Buffer {
  const b = new flatbuffers.Builder(1024)
  Message.finishMessageBuffer(b, message.pack(b))
  const bytes = Buffer.from(b.asUint8Array())
  if (bytes.length > maxFrame) throw Error('Remote worker frame exceeds 2 MiB.')
  const prefix = Buffer.alloc(4)
  prefix.writeUInt32LE(bytes.length)
  return Buffer.concat([prefix, bytes])
}

// The TypeScript runtime has no verifier and its unchecked reads can make an
// untrusted vector length drive unbounded unpacking. Bound generic accesses,
// including repeated/overlapping references, without duplicating schema fields.
class CheckedBuffer extends flatbuffers.ByteBuffer {
  private budget = maxFrame * 32
  private range(offset: number, size: number): void {
    if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(size) || offset < 0 || size < 0 ||
        offset > this.capacity() - size || (this.budget -= size) < 0) throw Error('Invalid remote worker buffer bounds.')
  }
  override readUint8(offset: number): number { this.range(offset, 1); return super.readUint8(offset) }
  override readUint16(offset: number): number { this.range(offset, 2); return super.readUint16(offset) }
  override readInt32(offset: number): number { this.range(offset, 4); return super.readInt32(offset) }
  override __vector_len(offset: number): number {
    const position = offset + this.readInt32(offset)
    const size = this.readInt32(position)
    this.range(position + 4, size)
    return size
  }
  override __string(offset: number, encoding?: flatbuffers.Encoding): string | Uint8Array {
    const position = offset + this.readInt32(offset)
    this.range(position + 4, this.readInt32(position))
    return super.__string(offset, encoding)
  }
}

export function decode(bytes: Buffer): MessageT {
  if (bytes.length < 12 || bytes.length > maxFrame) throw Error('Invalid remote worker frame size.')
  const bb = new CheckedBuffer(bytes)
  if (!Message.bufferHasIdentifier(bb)) throw Error('Invalid remote worker protocol identifier.')
  const result = Message.getRootAsMessage(bb).unpack()
  if (!result.payload || result.payloadType === Payload.NONE) throw Error('Missing remote worker payload.')
  return result
}

export function reply(bodyType: ReplyBody, body: ReplyT['body']): MessageT {
  return new MessageT(Payload.Reply, new ReplyT(bodyType, body))
}
export function failure(code: ErrorCode, message: string): MessageT {
  return reply(ReplyBody.Failure, new FailureT(code, message.slice(0, 4096)))
}
export function checkedReply(message: MessageT): ReplyT {
  if (message.payloadType !== Payload.Reply || !(message.payload instanceof ReplyT)) throw Error('Expected remote worker reply.')
  const r = message.payload
  if (r.bodyType === ReplyBody.Failure && r.body instanceof FailureT) throw Error(String(r.body.message))
  return r
}

// The carrier only needs a byte stream. TLS/SSH can wrap this unchanged.
export class Reader {
  private pending = Buffer.alloc(0)
  private readonly source: AsyncIterator<Buffer>
  constructor(stream: Readable) { this.source = stream[Symbol.asyncIterator]() }
  async exact(size: number): Promise<Buffer> {
    const result = Buffer.allocUnsafe(size)
    let offset = 0
    while (offset < size) {
      if (!this.pending.length) {
        const next = await this.source.next()
        if (next.done) throw Error('Remote worker closed the stream before replying.')
        this.pending = Buffer.from(next.value)
      }
      const n = Math.min(size - offset, this.pending.length)
      this.pending.copy(result, offset, 0, n)
      this.pending = this.pending.subarray(n)
      offset += n
    }
    return result
  }
  async message(): Promise<MessageT> {
    const n = (await this.exact(4)).readUInt32LE()
    if (!n || n > maxFrame) throw Error('Invalid remote worker frame length.')
    return decode(await this.exact(n))
  }
}
export function write(stream: Writable, message: MessageT): Promise<void> {
  return new Promise((resolve, reject) => stream.write(encode(message), (e) => e ? reject(e) : resolve()))
}
