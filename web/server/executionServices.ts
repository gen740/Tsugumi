import { once } from 'node:events'
import { createHash, createHmac, randomBytes, randomUUID, timingSafeEqual } from 'node:crypto'
import fs from 'node:fs'
import http from 'node:http'
import https from 'node:https'
import net from 'node:net'
import os from 'node:os'
import path from 'node:path'
import { Readable, Writable } from 'node:stream'
import { pipeline } from 'node:stream/promises'
import { L } from './i18n'

export interface ExecutionServiceCapabilities {
  serverId: string
  nativeSystem: string
  systems: string[]
  isolation: 'pid_namespace'
  resourceLimits: boolean
  maxConcurrentJobs: number
  workerContract: number
}

export interface ExecutionServiceConnection {
  id: string
  name: string
  endpoint: string
  capabilities: ExecutionServiceCapabilities
}

export interface ExecutionServiceRegistration {
  id: string
  name: string
  endpoint: string
  token?: string
}

type StoredExecutionService = ExecutionServiceConnection & { token: string }

const linuxSystems = new Set(['aarch64-linux', 'x86_64-linux'])
const operations = new Set(['probe', 'put_blob', 'get_blob', 'submit', 'status', 'cancel'])
const registryPath = (support: string) => path.join(support, 'execution-services.json')
const tokenPattern = /^[a-f0-9]{64}$/
class RegistrationChanged extends Error {}
const serviceIdPattern = /^[a-z][a-z0-9-]{0,31}$/
const workerContract = 2

function validServiceId(id: unknown): id is string {
  return typeof id === 'string' && id !== 'local' && serviceIdPattern.test(id)
}

function endpointUrl(value: unknown): string {
  const message = L('HTTPS の接続先、またはループバックアドレスの HTTP 接続先を入力してください。')
  if (typeof value !== 'string') throw Error(message)
  const endpoint = value.trim()
  if (endpoint.length > 2048 || !/^https?:\/\/[^/@?#\\\s]+\/?$/.test(endpoint)) throw Error(message)
  const url = new URL(endpoint)
  if (url.port && Number(url.port) === 0) throw Error(message)
  if (url.protocol === 'http:' && !/^http:\/\/(?:127\.0\.0\.1|\[::1\])(?::\d+)?\/?$/.test(endpoint)) throw Error(message)
  return url.origin
}

function registration(value: unknown): ExecutionServiceRegistration {
  const { id, name, endpoint, token } = (value ?? {}) as ExecutionServiceRegistration
  if (!validServiceId(id)) throw Error('Enter a distinct service ID of up to 32 lowercase letters, numbers and hyphens. The ID local is reserved.')
  if (typeof name !== 'string' || !name.trim() || [...name].length > 80 || /\p{Cc}/u.test(name)) throw Error('Enter a service name of up to 80 characters.')
  if (token !== undefined && (typeof token !== 'string' || !tokenPattern.test(token))) throw Error(L('認証トークンは小文字の16進数64文字です。'))
  return { id, name, endpoint: endpointUrl(endpoint), token }
}

function capabilities(value: unknown): ExecutionServiceCapabilities {
  const c = (value ?? {}) as ExecutionServiceCapabilities
  if (typeof c.serverId !== 'string' || !tokenPattern.test(c.serverId) ||
      typeof c.nativeSystem !== 'string' || !Array.isArray(c.systems) || !c.systems.includes(c.nativeSystem) ||
      !c.systems.every((system) => linuxSystems.has(system)) || c.isolation !== 'pid_namespace' ||
      typeof c.resourceLimits !== 'boolean' || !Number.isSafeInteger(c.maxConcurrentJobs) || c.maxConcurrentJobs < 1 || c.maxConcurrentJobs > 0xffffffff ||
      !Number.isSafeInteger(c.workerContract) || c.workerContract < 1) throw Error(L('実行デーモンが有効な実行環境情報を返しませんでした。'))
  return { serverId: c.serverId, nativeSystem: c.nativeSystem, systems: c.systems, isolation: c.isolation,
    resourceLimits: c.resourceLimits, maxConcurrentJobs: c.maxConcurrentJobs, workerContract: c.workerContract }
}

function loadStoredServices(support: string): StoredExecutionService[] {
  const file = registryPath(support)
  let fd: number
  try { fd = fs.openSync(file, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK) }
  catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return []
    throw error
  }
  try {
    const st = fs.fstatSync(fd)
    if (!st.isFile() || st.uid !== process.getuid?.() || (st.mode & 0o077) !== 0 || st.size > 1 << 20) {
      throw Error('The execution service registry must be a private host file.')
    }
    const value: unknown = JSON.parse(fs.readFileSync(fd, 'utf8'))
    if (!Array.isArray(value)) throw Error('Expected an array of execution services.')
    const entries = value.map((entry) => {
      const { id, name, endpoint, token } = registration(entry)
      if (!token) throw Error('The registered execution service has no authentication token.')
      return { id, name, endpoint, token, capabilities: capabilities(entry.capabilities) }
    })
    if (new Set(entries.map((entry) => entry.id)).size !== entries.length) throw Error('Each execution service must have a distinct ID.')
    return entries
  } finally { fs.closeSync(fd) }
}

export function loadExecutionServices(support: string): ExecutionServiceConnection[] {
  return loadStoredServices(support).map(({ id, name, endpoint, capabilities }) => ({ id, name, endpoint, capabilities }))
}

function saveStoredServices(support: string, entries: StoredExecutionService[]) {
  fs.mkdirSync(support, { recursive: true, mode: 0o700 })
  const temporary = path.join(support, '.execution-services-' + randomUUID())
  try {
    fs.writeFileSync(temporary, JSON.stringify(entries), { flag: 'wx', mode: 0o600 })
    fs.renameSync(temporary, registryPath(support))
  } finally { fs.rmSync(temporary, { force: true }) }
}

export async function registerExecutionService(support: string, value: unknown): Promise<ExecutionServiceConnection[]> {
  const entry = registration(value)
  const previous = loadStoredServices(support).find((current) => current.id === entry.id)
  const token = entry.token ?? (previous?.endpoint === entry.endpoint ? previous.token : undefined)
  if (!token) throw Error(L('この接続先の認証トークンを入力してください。'))
  const checked = await probeExecutionService({ ...entry, token })
  const entries = loadStoredServices(support)
  const current = entries.find((item) => item.id === entry.id)
  if (JSON.stringify(current) !== JSON.stringify(previous)) throw Error(L('登録内容が更新されました。もう一度設定を開いてください。'))
  saveStoredServices(support, [...entries.filter((item) => item.id !== entry.id), { ...checked, token }])
  return loadExecutionServices(support)
}

export function removeExecutionService(support: string, value: unknown): ExecutionServiceConnection[] {
  const id = (value as { id?: unknown } | undefined)?.id
  if (!validServiceId(id)) throw Error('Invalid execution service ID.')
  saveStoredServices(support, loadStoredServices(support).filter((entry) => entry.id !== id))
  return loadExecutionServices(support)
}

function executionBrokerKey(support: string): Buffer {
  const file = path.join(support, '.execution-broker-key')
  function readKey(): Buffer {
    const fd = fs.openSync(file, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK)
    try {
      const st = fs.fstatSync(fd)
      if (!st.isFile() || st.uid !== process.getuid?.() || (st.mode & 0o077) !== 0 || st.size !== 32) {
        throw Error('The execution broker key must be a private host file.')
      }
      return fs.readFileSync(fd)
    } finally { fs.closeSync(fd) }
  }
  try { return readKey() }
  catch (e) { if ((e as NodeJS.ErrnoException).code !== 'ENOENT') throw e }
  const temporary = file + '-' + randomUUID()
  try {
    fs.writeFileSync(temporary, randomBytes(32), { flag: 'wx', mode: 0o600, flush: true })
    try { fs.linkSync(temporary, file) }
    catch (e) { if ((e as NodeJS.ErrnoException).code !== 'EEXIST') throw e }
    return readKey()
  } finally { fs.rmSync(temporary, { force: true }) }
}

function authorizationTag(key: Buffer, scope: string): Buffer {
  return createHmac('sha256', key).update('token\0').update(scope).digest()
}

/** Authorize a daemon before spawn using a project identity from the host
 *  filesystem. The signed token remains valid after the Web server restarts. */
export function authorizeExecutionProject(support: string, identity: string, control: string): string {
  const key = executionBrokerKey(support)
  const scope = createHmac('sha256', key).update('project\0').update(identity).digest('hex')
  const token = scope + '.' + authorizationTag(key, scope).toString('hex')
  const file = path.join(control, 'execution-broker-token')
  const temporary = file + '-' + randomUUID()
  try {
    fs.writeFileSync(temporary, token, { flag: 'wx', mode: 0o600 })
    fs.renameSync(temporary, file)
  } finally { fs.rmSync(temporary, { force: true }) }
  return file
}

function projectScope(key: Buffer, token: unknown): string {
  const match = typeof token === 'string' && /^([a-f0-9]{64})\.([a-f0-9]{64})$/.exec(token)
  if (!match || !timingSafeEqual(authorizationTag(key, match[1]), Buffer.from(match[2], 'hex'))) {
    throw Error('The project is not authorized to use this execution broker.')
  }
  return match[1]
}

type Header = Record<string, unknown>
const headerLimit = 64 * 1024

function frame(header: Header): Buffer {
  const json = Buffer.from(JSON.stringify(header))
  if (!json.length || json.length > headerLimit) throw Error('Invalid execution service header size.')
  const length = Buffer.alloc(4)
  length.writeUInt32BE(json.length)
  return Buffer.concat([length, json])
}

function bodySize(header: Header): number {
  const size = header.size ?? 0
  if (typeof size !== 'number' || !Number.isSafeInteger(size) || size < 0 || size > 64 * 2 ** 30) throw Error('Invalid execution service body size.')
  return size
}

class FramedReader {
  private pending = Buffer.alloc(0)
  private readBytes = 0
  get consumed(): number { return this.readBytes }
  private readonly source: AsyncIterator<Buffer>

  constructor(stream: Readable) { this.source = stream[Symbol.asyncIterator]() }

  private async exact(size: number): Promise<Buffer> {
    const result = Buffer.allocUnsafe(size)
    let offset = 0
    while (offset < size) {
      if (!this.pending.length) {
        const next = await this.source.next()
        if (next.done) throw Error('The execution service closed the connection before replying.')
        this.pending = Buffer.from(next.value)
      }
      const take = Math.min(size - offset, this.pending.length)
      this.pending.copy(result, offset, 0, take)
      offset += take
      this.pending = this.pending.subarray(take)
    }
    this.readBytes += size
    return result
  }

  async header(): Promise<Header> {
    const length = (await this.exact(4)).readUInt32BE()
    if (!length || length > headerLimit) throw Error('Invalid execution service header size.')
    const header = JSON.parse((await this.exact(length)).toString('utf8')) as Header
    if (header?.version !== 1) throw Error('Unsupported execution service protocol version.')
    bodySize(header)
    return header
  }

  async *body(size: number): AsyncGenerator<Buffer> {
    while (size > 0) {
      const n = Math.min(64 * 1024, size)
      yield await this.exact(n)
      size -= n
    }
  }
}

async function* sendFrame(header: Header, body?: FramedReader): AsyncGenerator<Buffer> {
  yield frame(header)
  if (body) yield* body.body(bodySize(header))
}

function write(stream: Writable, data: Buffer): Promise<void> {
  return new Promise((resolve, reject) => stream.write(data, (error) => error ? reject(error) : resolve()))
}

async function remote(entry: Pick<StoredExecutionService, 'endpoint' | 'token'>, header: Header, input?: FramedReader, socket?: net.Socket): Promise<Header> {
  const url = new URL('/rpc', entry.endpoint)
  const total = frame(header).length + bodySize(header)
  const transport = url.protocol === 'https:' ? https : http
  const pinned = typeof header.server_id === 'string' ? header.server_id : undefined
  const waitForContinue = bodySize(header) > 0
  let continued = !waitForContinue
  let request: http.ClientRequest | undefined
  let connectTimer: ReturnType<typeof setTimeout> | undefined
  const disconnect = () => request?.destroy()
  socket?.once('close', disconnect)
  try {
    const response = await new Promise<http.IncomingMessage>((resolve, reject) => {
      request = transport.request(url, { method: 'POST', agent: false, headers: {
        'Authorization': 'Bearer ' + entry.token,
        'Content-Type': 'application/octet-stream',
        'Content-Length': total,
        'Accept-Encoding': 'identity',
        ...(pinned ? { 'X-Tsugumi-Server': pinned } : {}),
        ...(waitForContinue ? { Expect: '100-continue' } : {}),
      } }, resolve)
      request.once('error', reject)
      connectTimer = setTimeout(() => request?.destroy(Error('The execution daemon did not connect in time.')), 10_000)
      request.once('socket', (stream) => {
        if (stream.connecting) {
          stream.once(url.protocol === 'https:' ? 'secureConnect' : 'connect', () => clearTimeout(connectTimer))
        } else {
          clearTimeout(connectTimer)
        }
      })
      request.setTimeout(60_000, () => request?.destroy(Error('The execution daemon connection timed out.')))
      const send = () => {
        continued = true
        void pipeline(Readable.from(sendFrame(header, input)), request!).catch(reject)
      }
      if (waitForContinue) {
        request.once('continue', send)
        request.flushHeaders()
      } else {
        send()
      }
    })
    if (pinned && response.statusCode === 409 && (!waitForContinue || !continued)) {
      throw new RegistrationChanged('The execution daemon at this endpoint has changed. Verify its connection in settings.')
    }
    if (response.statusCode !== 200) {
      throw Error(L('実行デーモンへの接続に失敗しました（HTTP {0}）。', response.statusCode ?? ''))
    }
    if (!continued) throw Error('The execution daemon did not accept the request body.')
    const length = Number(response.headers['content-length'])
    if (!Number.isSafeInteger(length) || length < 5 || response.headers['content-encoding']) {
      throw Error('Invalid execution daemon response length.')
    }
    const reader = new FramedReader(response)
    const reply = await reader.header()
    const size = bodySize(reply)
    if (reader.consumed + size !== length || (!socket && size !== 0)) throw Error('Invalid execution daemon response size.')
    if (socket) {
      await write(socket, frame(reply))
      for await (const chunk of reader.body(size)) await write(socket, chunk)
    }
    return reply
  } finally {
    clearTimeout(connectTimer)
    socket?.removeListener('close', disconnect)
    disconnect()
  }
}

async function probeExecutionService(entry: ExecutionServiceRegistration & { token: string }): Promise<ExecutionServiceConnection> {
  const reply = await remote(entry, { version: 1, operation: 'probe', service: entry.id })
  if (reply.ok !== true) throw Error(typeof reply.error === 'string' ? reply.error : 'The execution service probe failed.')
  if (reply.executor !== 'daemon') throw Error(L('接続先で Tsugumi の実行デーモンを起動してください。'))
  if (reply.worker_contract !== workerContract) {
    const actual = typeof reply.worker_contract === 'number' ? reply.worker_contract : 'unreported'
    throw Error(`Execution daemon worker contract is ${actual}. Expected ${workerContract}. Rebuild the execution daemon with this Tsugumi revision.`)
  }
  const checked = capabilities({ serverId: reply.server_id, nativeSystem: reply.native_system, systems: reply.systems,
    isolation: reply.isolation, resourceLimits: reply.resource_limits, maxConcurrentJobs: reply.max_jobs, workerContract: reply.worker_contract })
  return { id: entry.id, name: entry.name, endpoint: entry.endpoint, capabilities: checked }
}

export async function startExecutionBroker(support: string): Promise<string> {
  const uid = process.getuid?.()
  if (uid === undefined) throw Error('Execution services require a Unix host.')
  const signingKey = executionBrokerKey(support)
  const key = createHash('sha256').update(support).digest('hex').slice(0, 12)
  const directory = path.join(os.tmpdir(), `tsugumi-exec-${uid}-${key}`)
  try { fs.mkdirSync(directory, { mode: 0o700 }) }
  catch (e) { if ((e as NodeJS.ErrnoException).code !== 'EEXIST') throw e }
  const st = fs.lstatSync(directory)
  if (!st.isDirectory() || st.uid !== uid || (st.mode & 0o077) !== 0) throw Error('The execution broker socket directory is not private.')
  const endpoint = path.join(directory, 's')

  const broker = net.createServer({ allowHalfOpen: true }, (socket) => {
    socket.on('error', () => {})
    socket.setTimeout(60_000, () => socket.destroy())
    void (async () => {
      let forwarding = false
      try {
        const reader = new FramedReader(socket)
        const header = await reader.header()
        const scope = projectScope(signingKey, header.broker_token)
        const entries = loadStoredServices(support)
        if (header.operation === 'catalog') {
          const services = entries.map(({ id, capabilities: c }) => ({
            id, native_system: c.nativeSystem, systems: c.systems, available: c.workerContract === workerContract, server_id: c.serverId,
            executor: 'daemon', isolation: c.isolation, worker_contract: c.workerContract,
            resource_limits: c.resourceLimits, max_jobs: c.maxConcurrentJobs,
          }))
          await write(socket, frame({ version: 1, ok: true, services }))
        } else {
          if (typeof header.operation !== 'string' || !operations.has(header.operation)) throw Error('Unknown execution service operation.')
          const entry = entries.find((e) => e.id === header.service)
          if (!entry || header.server_id !== entry.capabilities.serverId || entry.capabilities.workerContract !== workerContract) {
            throw new RegistrationChanged('The execution service registration changed. Verify its connection in settings.')
          }
          delete header.broker_token
          header.project_scope = scope
          header.server_id = entry.capabilities.serverId
          forwarding = true
          await remote(entry, header, reader, socket)
        }
      } catch (e) {
        // A transport interruption can happen after a request reached the gateway.
        // EOF leaves its outcome unconfirmed. Only the gateway can reject it.
        if (!forwarding || e instanceof RegistrationChanged) await write(socket, frame({
          version: 1, ok: false, error: (e as Error).message,
          ...(e instanceof RegistrationChanged ? { code: 'registration_changed' } : {}),
        })).catch(() => {})
      } finally { socket.end() }
    })()
  })
  const listen = () => new Promise<void>((resolve, reject) => {
    const failure = (error: Error) => reject(error)
    broker.once('error', failure)
    broker.listen(endpoint, () => { broker.removeListener('error', failure); resolve() })
  })
  try { await listen() }
  catch (e) {
    if ((e as NodeJS.ErrnoException).code !== 'EADDRINUSE' || !fs.lstatSync(endpoint).isSocket()) throw e
    const socket = net.createConnection(endpoint)
    try { await once(socket, 'connect'); return `unix:${endpoint}` }
    catch (error) { if ((error as NodeJS.ErrnoException).code !== 'ECONNREFUSED') throw error }
    finally { socket.destroy() }
    fs.unlinkSync(endpoint)
    await listen()
  }
  fs.chmodSync(endpoint, 0o600)
  broker.unref()
  return `unix:${endpoint}`
}
