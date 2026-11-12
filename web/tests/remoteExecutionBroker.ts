import assert from 'node:assert/strict'
import { once } from 'node:events'
import fs from 'node:fs'
import http from 'node:http'
import net from 'node:net'
import os from 'node:os'
import path from 'node:path'
import { authorizeExecutionProject, loadExecutionServices, registerExecutionService, removeExecutionService, startExecutionBroker } from '../server/executionServices'
import { executionScopeIdentity, projectExecutionScope } from '../server/env'

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-broker-test-'))
const token = 'a'.repeat(64)
const originalServerId = 'd'.repeat(64)
let serverId = originalServerId
let requestCount = 0
let rejectedBeforeUpload = 0
let bytesSentBeforeContinue = 0
let executor: string | undefined = 'daemon'
let workerContract: number | undefined = 1
const server = http.createServer((req, res) => { void handle(req, res) })
server.on('checkContinue', (req, res) => {
  if (req.headers.authorization !== `Bearer ${token}` || req.headers['x-tsugumi-server'] !== serverId) {
    rejectedBeforeUpload++
    req.on('data', (chunk: Buffer) => { bytesSentBeforeContinue += chunk.length })
    setTimeout(() => res.writeHead(409, { 'Content-Length': 0, Connection: 'close' }).end(), 150)
    return
  }
  res.writeContinue()
  void handle(req, res)
})

async function handle(req: http.IncomingMessage, res: http.ServerResponse) {
  try {
    requestCount++
    assert.equal(req.method, 'POST')
    assert.equal(req.url, '/rpc')
    assert.equal(req.headers.authorization, `Bearer ${token}`)
    const chunks: Buffer[] = []
    for await (const data of req) chunks.push(data as Buffer)
    const received = Buffer.concat(chunks)
    assert.equal(Number(req.headers['content-length']), received.length)
    const length = received.readUInt32BE()
    const request = JSON.parse(received.subarray(4, 4 + length).toString('utf8'))
    assert.equal(request.broker_token, undefined)
    const body = received.subarray(4 + length)
    if (request.project_scope) {
      assert.equal(req.headers['x-tsugumi-server'], serverId)
      assert.equal(request.server_id, serverId)
      assert.match(request.project_scope, /^[a-f0-9]{64}$/)
    } else {
      assert.equal(request.operation, 'probe')
      assert.equal(req.headers['x-tsugumi-server'], undefined)
    }
    if (request.operation === 'status') { res.destroy(); return }
    const reply = request.operation === 'probe'
      ? { version: 1, ok: true, executor, worker_contract: workerContract, server_id: serverId,
          native_system: 'x86_64-linux', systems: ['x86_64-linux'], isolation: 'pid_namespace', resource_limits: true, max_jobs: 4 }
      : { version: 1, ok: true, size: body.length, scope: request.project_scope }
    const json = Buffer.from(JSON.stringify(reply))
    const prefix = Buffer.alloc(4)
    prefix.writeUInt32BE(json.length)
    res.writeHead(200, { 'Content-Length': prefix.length + json.length + body.length })
    res.end(Buffer.concat([prefix, json, body]))
  } catch (error) {
    res.destroy(error as Error)
  }
}

let socketPath: string | undefined
try {
  server.listen(0, '127.0.0.1')
  await once(server, 'listening')
  const address = server.address() as net.AddressInfo
  const entry = { id: 'worker', name: 'Worker', endpoint: `http://127.0.0.1:${address.port}`, token }
  for (const endpoint of ['http://localhost:12345', 'http://192.0.2.1:12345', 'https://user:password@example.org', 'https://example.org/path']) {
    await assert.rejects(registerExecutionService(root, { ...entry, endpoint }))
  }
  assert.equal(requestCount, 0)
  const [verified] = await registerExecutionService(root, { ...entry, capabilities: { nativeSystem: 'unsupported' } })
  assert.deepEqual(verified.capabilities, { serverId, nativeSystem: 'x86_64-linux', systems: ['x86_64-linux'],
    isolation: 'pid_namespace', resourceLimits: true, maxConcurrentJobs: 4, workerContract: 1 })
  assert.equal(JSON.stringify(verified).includes(token), false)
  assert.deepEqual(loadExecutionServices(root), [verified])
  const registryPath = path.join(root, 'execution-services.json')
  assert.equal(fs.statSync(registryPath).mode & 0o777, 0o600)
  const beforeRebind = requestCount
  await assert.rejects(registerExecutionService(root, { ...entry, endpoint: 'http://127.0.0.1:1', token: undefined }), /認証トークン/)
  assert.equal(requestCount, beforeRebind, 'a saved token cannot be reused at a different endpoint')
  executor = undefined
  await assert.rejects(registerExecutionService(root, entry), /実行デーモンを起動/)
  executor = 'daemon'
  workerContract = 99
  await assert.rejects(registerExecutionService(root, entry), /worker contract is 99.*Rebuild/)
  workerContract = undefined
  await assert.rejects(registerExecutionService(root, entry), /worker contract is unreported.*Rebuild/)
  workerContract = 1
  assert.deepEqual(loadExecutionServices(root), [verified], 'failed probes preserve the verified registry entry')

  const a = path.join(root, 'project-a'), renamed = path.join(root, 'project-renamed'), b = path.join(root, 'project-b')
  fs.mkdirSync(a)
  fs.mkdirSync(b)
  const generation = { dev: 1n, ino: 2n }
  assert.equal(executionScopeIdentity({ ...generation, birthtimeNs: 0n }), undefined)
  assert.notEqual(executionScopeIdentity({ ...generation, birthtimeNs: 10n }), executionScopeIdentity({ ...generation, birthtimeNs: 11n }))
  const authorize = (project: string) => {
    const identity = projectExecutionScope(project)
    assert.ok(identity, 'the broker test requires a filesystem with creation times')
    return fs.readFileSync(authorizeExecutionProject(root, identity, root), 'utf8')
  }
  const aToken = authorize(a)
  const bToken = authorize(b)
  fs.writeFileSync(path.join(a, 'new-card.json'), '{}')
  assert.equal(authorize(a), aToken, 'editing preserves the broker scope')
  fs.renameSync(a, renamed)
  assert.equal(authorize(renamed), aToken, 'renaming on the same filesystem preserves the broker scope')
  assert.notEqual(aToken, bToken)
  assert.equal(fs.statSync(path.join(root, '.execution-broker-key')).mode & 0o777, 0o600)
  assert.equal(fs.statSync(path.join(root, 'execution-broker-token')).mode & 0o777, 0o600)
  socketPath = (await startExecutionBroker(root)).slice('unix:'.length)

  function call(operation: string, body = Buffer.alloc(0), auth = aToken, pin = originalServerId): Promise<Buffer> {
    const json = Buffer.from(JSON.stringify({
      version: 1, operation, service: 'worker', broker_token: auth, project_scope: '0'.repeat(64), server_id: pin, size: body.length,
    }))
    const prefix = Buffer.alloc(4)
    prefix.writeUInt32BE(json.length)
    return new Promise((resolve, reject) => {
      const client = net.createConnection(socketPath!)
      const chunks: Buffer[] = []
      client.on('connect', () => client.end(Buffer.concat([prefix, json, body])))
      client.on('data', (data) => chunks.push(data))
      client.on('end', () => resolve(Buffer.concat(chunks)))
      client.on('error', reject)
    })
  }
  const header = (reply: Buffer) => JSON.parse(reply.subarray(4, 4 + reply.readUInt32BE()).toString('utf8'))
  assert.deepEqual(header(await call('catalog')).services, [
    { id: 'worker', native_system: 'x86_64-linux', systems: ['x86_64-linux'], available: true,
      server_id: originalServerId, executor: 'daemon', isolation: 'pid_namespace', resource_limits: true, max_jobs: 4, worker_contract: 1 },
  ])
  const input = Buffer.alloc(192 * 1024, 0xd3)
  const response = await call('put_blob', input)
  assert.deepEqual(response.subarray(4 + response.readUInt32BE()), input)
  assert.equal(header(response).scope, aToken.split('.')[0], 'the broker overwrites a client-supplied scope')
  assert.equal(header(await call('put_blob', input, bToken)).scope, bToken.split('.')[0])
  const bad = await call('put_blob', Buffer.alloc(0), bToken.split('.')[0] + '.' + aToken.split('.')[1])
  assert.equal(header(bad).ok, false, 'a token cannot be retargeted to another project')
  assert.equal((await call('status')).length, 0, 'a lost response leaves the job outcome unconfirmed')

  const beforeSwap = requestCount
  serverId = 'e'.repeat(64)
  assert.equal(header(await call('put_blob', input)).code, 'registration_changed')
  assert.equal(rejectedBeforeUpload, 1)
  assert.equal(bytesSentBeforeContinue, 0, 'a silent or refusing endpoint receives no frame bytes')
  assert.equal(requestCount, beforeSwap)
  await registerExecutionService(root, { ...entry, token: undefined })
  const afterRegister = requestCount
  assert.equal(header(await call('status')).code, 'registration_changed')
  assert.equal(header(await call('put_blob', Buffer.alloc(0))).code, 'registration_changed')
  assert.equal(requestCount, afterRegister, 'a replaced registration is rejected before dialing')
  assert.deepEqual(removeExecutionService(root, { id: 'worker' }), [])
} finally {
  server.close()
  if (socketPath) fs.rmSync(path.dirname(socketPath), { recursive: true, force: true })
  fs.rmSync(root, { recursive: true, force: true })
}

console.log('Remote execution broker: authenticated registration, framing, private registry, server pins, and project scopes passed')
