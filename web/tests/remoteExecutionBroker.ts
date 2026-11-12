import assert from 'node:assert/strict'
import fs from 'node:fs'
import net from 'node:net'
import os from 'node:os'
import path from 'node:path'
import { authorizeExecutionProject, probeExecutionService, saveExecutionServices, startExecutionBroker } from '../server/executionServices'
import { executionScopeIdentity, projectExecutionScope } from '../server/env'

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-broker-test-'))
const oldPath = process.env.PATH
let socketPath: string | undefined
try {
  const entry = { id: 'worker', name: 'Worker', sshHost: 'trusted', gateway: '/tmp/worker/bin/tsugumi-remote-gateway' }
  const ssh = `#!${process.execPath}
const fs = require('node:fs')
const args = process.argv.slice(2)
if (!args.includes('StrictHostKeyChecking=yes') || !args.includes('ForwardAgent=no')) process.exit(2)
if (args.at(-2) !== 'trusted' || args.at(-1) !== "'/tmp/worker/bin/tsugumi-remote-gateway'") process.exit(3)
const data = fs.readFileSync(0), length = data.readUInt32BE()
const request = JSON.parse(data.subarray(4, 4 + length))
if (request.broker_token) process.exit(4)
if (request.operation === 'status') process.exit(1) // transport closed before a response
const probe = request.operation === 'probe'
if (!probe && !/^[a-f0-9]{64}$/.test(request.project_scope)) process.exit(5)
const body = probe ? Buffer.alloc(0) : data.subarray(4 + length)
const contract = process.env.TEST_WORKER_CONTRACT
const reply = probe ? { version: 1, ok: true, worker_contract: contract === 'missing' ? undefined : Number(contract ?? 1),
    native_system: 'x86_64-linux', systems: ['x86_64-linux'] }
  : { version: 1, ok: true, size: body.length, scope: request.project_scope }
const json = Buffer.from(JSON.stringify(reply)), prefix = Buffer.alloc(4)
prefix.writeUInt32BE(json.length)
fs.writeFileSync(1, Buffer.concat([prefix, json, body]))
`
  fs.writeFileSync(path.join(root, 'ssh'), ssh, { mode: 0o700 })
  process.env.PATH = root + path.delimiter + (oldPath ?? '')
  assert.throws(() => saveExecutionServices(root, [{ ...entry, gateway: '/tmp/gateway; touch bad' }]), /absolute path/)
  const verified = await probeExecutionService(entry)
  assert.deepEqual(verified.systems, ['x86_64-linux'])
  process.env.TEST_WORKER_CONTRACT = '99'
  await assert.rejects(probeExecutionService(entry), /worker contract is 99.*Rebuild the gateway/)
  process.env.TEST_WORKER_CONTRACT = 'missing'
  await assert.rejects(probeExecutionService(entry), /worker contract is unreported.*Rebuild the gateway/)
  delete process.env.TEST_WORKER_CONTRACT
  saveExecutionServices(root, [verified])
  assert.equal(fs.statSync(path.join(root, 'execution-services.json')).mode & 0o777, 0o600)
  const a = path.join(root, 'project-a'), renamed = path.join(root, 'project-renamed'), b = path.join(root, 'project-b')
  fs.mkdirSync(a)
  fs.mkdirSync(b)
  const generation = { dev: 1n, ino: 2n }
  assert.equal(executionScopeIdentity({ ...generation, birthtimeNs: 0n }), undefined)
  assert.notEqual(executionScopeIdentity({ ...generation, birthtimeNs: 10n }), executionScopeIdentity({ ...generation, birthtimeNs: 11n }))
  const token = (project: string) => {
    const identity = projectExecutionScope(project)
    assert.ok(identity, 'the broker test requires a filesystem with creation times')
    return fs.readFileSync(authorizeExecutionProject(root, identity, root), 'utf8')
  }
  const aToken = token(a)
  const bToken = token(b)
  fs.writeFileSync(path.join(a, 'new-card.json'), '{}')
  assert.equal(token(a), aToken, 'editing the project directory preserves its broker scope')
  fs.renameSync(a, renamed)
  assert.equal(token(renamed), aToken, 'renaming a project on the same filesystem preserves its broker scope')
  assert.notEqual(aToken, bToken)
  assert.equal(fs.statSync(path.join(root, '.execution-broker-key')).mode & 0o777, 0o600)
  assert.equal(fs.statSync(path.join(root, 'execution-broker-token')).mode & 0o777, 0o600)
  socketPath = (await startExecutionBroker(root)).slice('unix:'.length)

  function call(operation: string, body = Buffer.alloc(0), auth = aToken): Promise<Buffer> {
    const json = Buffer.from(JSON.stringify({ version: 1, operation, service: 'worker', broker_token: auth, project_scope: '0'.repeat(64), size: body.length }))
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
  const catalog = await call('catalog')
  assert.deepEqual(JSON.parse(catalog.subarray(4)).services, [
    { id: 'worker', native_system: 'x86_64-linux', systems: ['x86_64-linux'], available: true },
  ])
  const input = Buffer.alloc(192 * 1024, 0xd3)
  const response = await call('put_blob', input)
  assert.deepEqual(response.subarray(4 + response.readUInt32BE()), input)
  const scope = (reply: Buffer) => JSON.parse(reply.subarray(4, 4 + reply.readUInt32BE())).scope
  assert.equal(scope(response), aToken.split('.')[0], 'the broker overwrites a client-supplied scope')
  assert.equal(scope(await call('put_blob', input, bToken)), bToken.split('.')[0])
  const bad = await call('put_blob', Buffer.alloc(0), bToken.split('.')[0] + '.' + aToken.split('.')[1])
  assert.equal(JSON.parse(bad.subarray(4)).ok, false, 'a token cannot be retargeted to another project')
  assert.equal((await call('status')).length, 0, 'a lost SSH reply leaves the job outcome unconfirmed')
} finally {
  delete process.env.TEST_WORKER_CONTRACT
  if (oldPath === undefined) delete process.env.PATH
  else process.env.PATH = oldPath
  if (socketPath) fs.rmSync(path.dirname(socketPath), { recursive: true, force: true })
  fs.rmSync(root, { recursive: true, force: true })
}

console.log('Remote execution broker: framing, private registry, SSH arguments, project scopes and disconnect passed')
