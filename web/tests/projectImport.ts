import assert from 'node:assert/strict'
import * as fb from 'flatbuffers'
import { importProject } from '../server/projectImport'
import { encodeRequest, decodeReply } from '../server/serviceWire'
import { Message, MessageT } from '../src/gen/tsugumi/host/message'
import { ProjectFilesT } from '../src/gen/tsugumi/host/project-files'
import { ProjectFileT } from '../src/gen/tsugumi/host/project-file'
import { ReplyT } from '../src/gen/tsugumi/host/reply'
import { ReplyPayload } from '../src/gen/tsugumi/host/reply-payload'
import { Payload } from '../src/gen/tsugumi/host/payload'

const calls: any[] = []
const result = await importProject('/source', async <T>(method: string, fields: any): Promise<T> => {
  calls.push({ method, fields })
  const encoded = encodeRequest(method, fields, 1n, true)
  const request = (Message.getRootAsMessage(new fb.ByteBuffer(encoded)).unpack().payload as any).value
  assert.equal(request.path, '/source')
  assert.equal(request.fileIndex, fields.file_index)
  assert.equal(request.offset, BigInt(fields.offset))
  const n = calls.length
  const file = new ProjectFileT(n === 1 ? 'tsugumi.json' : 'definitions/card/src/.config', n === 1 ? [...Buffer.from('{}')] : n === 2 ? [255, 0] : [128])
  const value = new ProjectFilesT('source', [file], n === 1 ? 1 : 2, n === 2 ? 2n : 0n, n === 3)
  const b = new fb.Builder()
  Message.finishMessageBuffer(b, new MessageT(1n, Payload.Reply, new ReplyT(ReplyPayload.ProjectFiles, value)).pack(b))
  return decodeReply(b.asUint8Array(), true).value as T
})
assert.deepEqual(calls.map((c) => c.fields), [
  { path: '/source', file_index: 0, offset: 0 }, { path: '/source', file_index: 1, offset: 0 }, { path: '/source', file_index: 2, offset: 2 },
])
assert.deepEqual(Buffer.from(result.files['definitions/card/src/.config'], 'base64'), Buffer.from([255, 0, 128]))
assert.equal(Buffer.from(result.files['tsugumi.json'], 'base64').toString(), '{}')
console.log('Host project import wire, continuation and binary transport: passed')
