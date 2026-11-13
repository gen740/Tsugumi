import assert from 'node:assert/strict'
import { once } from 'node:events'
import fs from 'node:fs'
import net from 'node:net'
import os from 'node:os'
import path from 'node:path'
import { authorizeExecutionProject, loadExecutionServices, registerExecutionService, removeExecutionService, startExecutionBroker } from '../server/executionServices'
import { executionScopeIdentity, projectExecutionScope } from '../server/env'
import * as R from '../server/remoteWorker'
import { BlobChunkT } from '../src/gen/tsugumi/remote-worker/blob-chunk'
import { PutBlobRequestT } from '../src/gen/tsugumi/remote-worker/put-blob-request'
import { StatusRequestT } from '../src/gen/tsugumi/remote-worker/status-request'
import { CatalogRequestT } from '../src/gen/tsugumi/remote-worker/catalog-request'

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-broker-test-'))
const token = 'a'.repeat(64)
const originalServer = 'd'.repeat(64)
let serverId = originalServer, workerContract = 3, requestCount = 0
const scopes: (string | Uint8Array | null)[] = []
const server = net.createServer({ allowHalfOpen: true }, (socket) => {
  socket.on('error', () => {})
  void (async () => {
    try {
      const message = await new R.Reader(socket).message()
      const q = message.payload as R.RequestT
      requestCount++
      assert.equal(q.token, token)
      if (q.projectScope) {
        assert.equal(q.serverId, originalServer)
        scopes.push(q.projectScope)
      } else assert.equal(q.bodyType, R.RequestBody.ProbeRequest)
      if (q.bodyType === R.RequestBody.StatusRequest) return
      if (q.serverId && q.serverId !== serverId) {
        await R.write(socket, R.failure(R.ErrorCode.InstanceChanged, 'worker changed'))
      } else if (q.bodyType === R.RequestBody.ProbeRequest) {
        await R.write(socket, R.reply(R.ReplyBody.Capabilities,
          new R.CapabilitiesT(serverId, null, 'x86_64-linux', ['x86_64-linux'], true, 4, workerContract)))
      } else {
        const body = q.body as PutBlobRequestT
        await R.write(socket, R.reply(R.ReplyBody.BlobChunk, new BlobChunkT(body.digest, body.total, body.offset, body.bytes)))
      }
    } finally { socket.end() }
  })().catch(() => socket.destroy())
})
let socketPath: string | undefined
try {
  server.listen(0, '127.0.0.1')
  await once(server, 'listening')
  const address = server.address() as net.AddressInfo
  const entry = { id: 'worker', name: 'Worker', endpoint: `tcp://127.0.0.1:${address.port}`, token }
  for (const endpoint of ['http://127.0.0.1:1234', 'tcp://localhost:1234', 'tcp://192.0.2.1:1234', 'tcp://127.0.0.1:0', 'unix:relative']) {
    await assert.rejects(registerExecutionService(root, { ...entry, endpoint }))
  }
  assert.equal(requestCount, 0)
  const [verified] = await registerExecutionService(root, entry)
  assert.equal(verified.capabilities.workerContract, R.contract)
  assert.equal(JSON.stringify(verified).includes(token), false)
  assert.deepEqual(loadExecutionServices(root), [verified])
  assert.equal(fs.statSync(path.join(root, 'execution-services.json')).mode & 0o777, 0o600)
  const beforeRebind = requestCount
  await assert.rejects(registerExecutionService(root, { ...entry, endpoint: 'tcp://127.0.0.1:1', token: undefined }))
  assert.equal(requestCount, beforeRebind)
  workerContract = 99
  await assert.rejects(registerExecutionService(root, entry), /Rebuild the remote worker/)
  workerContract = 3
  assert.deepEqual(loadExecutionServices(root), [verified])
  const a = path.join(root, 'project-a'), renamed = path.join(root, 'project-renamed'), b = path.join(root, 'project-b')
  fs.mkdirSync(a); fs.mkdirSync(b)
  assert.equal(executionScopeIdentity({ dev: 1n, ino: 2n, birthtimeNs: 0n }), undefined)
  const authorize = (project: string) => {
    const identity = projectExecutionScope(project)
    assert.ok(identity)
    return fs.readFileSync(authorizeExecutionProject(root, identity, root), 'utf8')
  }
  const aToken = authorize(a), bToken = authorize(b)
  fs.renameSync(a, renamed)
  assert.equal(authorize(renamed), aToken)
  assert.notEqual(aToken, bToken)
  socketPath = (await startExecutionBroker(root)).slice(5)
  async function call(kind: R.RequestBody, body: R.RequestT['body'], auth = aToken, pin = originalServer): Promise<R.MessageT | undefined> {
    const q = new R.RequestT(auth, 'worker', pin, null, '0'.repeat(64), '1'.repeat(64), kind, body)
    const socket = net.createConnection(socketPath!)
    socket.on('error', () => {})
    try {
      await once(socket, 'connect')
      await R.write(socket, new R.MessageT(R.Payload.Request, q))
      socket.end()
      try { return await new R.Reader(socket).message() } catch { return undefined }
    } finally { socket.destroy() }
  }
  const catalog = R.checkedReply((await call(R.RequestBody.CatalogRequest, new CatalogRequestT()))!).body as R.CatalogT
  assert.equal(catalog.services[0].id, 'worker')
  const input = Array.from(Buffer.alloc(192 * 1024, 0xd3))
  const data = new PutBlobRequestT('c'.repeat(64), BigInt(input.length), 0n, input)
  const response = R.checkedReply((await call(R.RequestBody.PutBlobRequest, data))!).body as BlobChunkT
  assert.deepEqual(response.bytes, input)
  assert.equal(scopes.at(-1), aToken.split('.')[0], 'broker must overwrite a caller-supplied project scope')
  await call(R.RequestBody.PutBlobRequest, data, bToken)
  assert.equal(scopes.at(-1), bToken.split('.')[0])
  const bad = await call(R.RequestBody.PutBlobRequest, data, bToken.split('.')[0] + '.' + aToken.split('.')[1])
  assert.equal((bad?.payload as R.ReplyT).bodyType, R.ReplyBody.Failure)
  assert.equal(await call(R.RequestBody.StatusRequest, new StatusRequestT()), undefined, 'lost replies must remain unconfirmed')
  serverId = 'e'.repeat(64)
  const changed = await call(R.RequestBody.PutBlobRequest, data)
  assert.equal((changed?.payload as R.ReplyT).bodyType, R.ReplyBody.Failure)
  await registerExecutionService(root, { ...entry, token: undefined })
  const before = requestCount
  const stale = await call(R.RequestBody.StatusRequest, new StatusRequestT())
  assert.equal((stale?.payload as R.ReplyT).bodyType, R.ReplyBody.Failure)
  assert.equal(requestCount, before, 'stale registration must fail before dialing')
  assert.deepEqual(removeExecutionService(root, { id: 'worker' }), [])
} finally {
  server.close()
  if (socketPath) fs.rmSync(path.dirname(socketPath), { recursive: true, force: true })
  fs.rmSync(root, { recursive: true, force: true })
}
console.log('Remote worker broker: generated framing, private registration, project scopes and recovery failures passed')
