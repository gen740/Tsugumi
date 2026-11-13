import { once } from 'node:events'
import { createHash, createHmac, randomBytes, randomUUID, timingSafeEqual } from 'node:crypto'
import fs from 'node:fs'
import net from 'node:net'
import os from 'node:os'
import path from 'node:path'
import { L } from './i18n'
import * as R from './remoteWorker'

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
const registryPath = (support: string) => path.join(support, 'execution-services.json')
const tokenPattern = /^[a-f0-9]{64}$/
class RegistrationChanged extends Error {}
const serviceIdPattern = /^[a-z][a-z0-9-]{0,31}$/
const workerContract = R.contract

function validServiceId(id: unknown): id is string {
  return typeof id === 'string' && id !== 'local' && serviceIdPattern.test(id)
}

function endpointUrl(value: unknown): string {
  if (typeof value !== 'string') throw Error('Enter a Unix socket or loopback TCP endpoint.')
  const endpoint = value.trim()
  if (endpoint.length > 2048) throw Error('Remote worker endpoint is too long.')
  if (/^unix:\/[^/\\\s%?#][^\\\s%?#]*$/.test(endpoint) && !/\p{Cc}/u.test(endpoint)) return endpoint
  if (!/^tcp:\/\/(?:127\.0\.0\.1|\[::1\]):[0-9]+$/.test(endpoint)) throw Error('Enter unix:/absolute/socket or tcp://127.0.0.1:8765.')
  const url = new URL(endpoint)
  if (!url.port || Number(url.port) > 65535 || Number(url.port) < 1) throw Error('Invalid TCP port.')
  return endpoint
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

async function remote(entry: Pick<StoredExecutionService, 'endpoint' | 'token'>, request: R.RequestT, guest?: net.Socket): Promise<R.MessageT> {
  const endpoint = endpointUrl(entry.endpoint)
  const socket = endpoint.startsWith('unix:') ? net.createConnection(endpoint.slice(5))
    : net.createConnection({ host: new URL(endpoint).hostname.replace(/^\[|\]$/g, ''), port: Number(new URL(endpoint).port) })
  const timer = setTimeout(() => socket.destroy(Error('Remote worker did not connect in time.')), 10_000)
  socket.setTimeout(60_000, () => socket.destroy(Error('Remote worker connection timed out.')))
  socket.on('error', () => {})
  const disconnect = () => socket.destroy()
  guest?.once('close', disconnect)
  try {
    await once(socket, 'connect')
    clearTimeout(timer)
    request.token = entry.token
    await R.write(socket, new R.MessageT(R.Payload.Request, request))
    return await new R.Reader(socket).message()
  } finally {
    clearTimeout(timer)
    guest?.removeListener('close', disconnect)
    socket.destroy()
  }
}

function wireCapabilities(c: ExecutionServiceCapabilities): R.CapabilitiesT {
  return new R.CapabilitiesT(c.serverId, null, c.nativeSystem, c.systems, c.resourceLimits, c.maxConcurrentJobs, c.workerContract)
}
async function probeExecutionService(entry: ExecutionServiceRegistration & { token: string }): Promise<ExecutionServiceConnection> {
  const q = new R.RequestT()
  q.bodyType = R.RequestBody.ProbeRequest
  q.body = new R.ProbeRequestT()
  const reply = R.checkedReply(await remote(entry, q))
  if (reply.bodyType !== R.ReplyBody.Capabilities || !(reply.body instanceof R.CapabilitiesT)) throw Error('Remote worker did not report its capabilities.')
  const c = reply.body
  if (c.workerContract !== workerContract) throw Error(`Rebuild the remote worker with this Tsugumi revision (expected contract ${workerContract}).`)
  const checked = capabilities({ serverId: c.serverId, nativeSystem: c.nativeSystem, systems: c.systems,
    isolation: 'pid_namespace', resourceLimits: c.resourceLimits, maxConcurrentJobs: c.maxJobs, workerContract: c.workerContract })
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
        const message = await new R.Reader(socket).message()
        if (message.payloadType !== R.Payload.Request || !(message.payload instanceof R.RequestT)) throw Error('Expected remote worker request.')
        const request = message.payload
        const scope = projectScope(signingKey, request.token)
        const entries = loadStoredServices(support)
        if (request.bodyType === R.RequestBody.CatalogRequest) {
          const services = entries.map(({ id, capabilities: c }) => new R.TargetT(id, c.workerContract === workerContract, wireCapabilities(c)))
          await R.write(socket, R.reply(R.ReplyBody.Catalog, new R.CatalogT(services)))
        } else {
          if (request.bodyType === R.RequestBody.NONE || !request.body) throw Error('Missing remote worker request.')
          const entry = entries.find((e) => e.id === request.service)
          if (!entry || request.serverId !== entry.capabilities.serverId || entry.capabilities.workerContract !== workerContract) {
            throw new RegistrationChanged('The execution service registration changed. Verify its connection in settings.')
          }
          request.projectScope = scope
          request.serverId = entry.capabilities.serverId
          forwarding = true
          await R.write(socket, await remote(entry, request, socket))
        }
      } catch (e) {
        // A transport interruption leaves the submission's outcome unconfirmed.
        if (!forwarding || e instanceof RegistrationChanged) await R.write(socket, R.failure(
          e instanceof RegistrationChanged ? R.ErrorCode.RegistrationChanged : R.ErrorCode.Invalid, (e as Error).message)).catch(() => {})
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
