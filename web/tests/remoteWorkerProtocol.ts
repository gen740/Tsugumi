import assert from 'node:assert/strict'
import * as R from '../server/remoteWorker'
import { PutBlobRequestT } from '../src/gen/tsugumi/remote-worker/put-blob-request'

const q = new R.RequestT()
q.bodyType = R.RequestBody.PutBlobRequest
q.body = new PutBlobRequestT('a'.repeat(64), 4n, 0n, [0, 1, 128, 255])
const bytes = R.encode(new R.MessageT(R.Payload.Request, q))
assert.equal(bytes.readUInt32LE(), bytes.length - 4)
const result = R.decode(bytes.subarray(4)).payload as R.RequestT
assert.deepEqual((result.body as PutBlobRequestT).bytes, [0, 1, 128, 255])
const malformed = Buffer.from(bytes.subarray(4))
malformed.writeUInt32LE(0x7fffffff, 0)
assert.throws(() => R.decode(malformed), /bounds/)
assert.throws(() => R.decode(Buffer.alloc(24)))
assert.throws(() => R.decode(Buffer.alloc(R.maxFrame + 1)))
console.log('Remote worker generated protocol: roundtrip and malformed bounds passed')
