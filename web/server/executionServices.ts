import { spawn } from 'node:child_process'
import { once } from 'node:events'
import { createHash, createHmac, randomBytes, randomUUID, timingSafeEqual } from 'node:crypto'
import fs from 'node:fs'
import net from 'node:net'
import os from 'node:os'
import path from 'node:path'
import { Readable, Writable } from 'node:stream'
import { pipeline } from 'node:stream/promises'

export interface ExecutionServiceConnection {
  id: string
  name: string
  sshHost: string
  gateway?: string
  nativeSystem?: string
  systems?: string[]
}

const linuxSystems = new Set(['aarch64-linux', 'x86_64-linux'])
const operations = new Set(['probe', 'put_blob', 'get_blob', 'submit', 'status', 'cancel'])
const registryPath = (support: string) => path.join(support, 'execution-services.json')

function validateConnection(value: unknown): ExecutionServiceConnection {
  const { id, name, sshHost, gateway, nativeSystem, systems } = (value ?? {}) as ExecutionServiceConnection
  if (typeof id !== 'string' || id === 'local' || !/^[a-z][a-z0-9-]{0,31}$/.test(id)) throw Error('Enter a distinct service ID of up to 32 lowercase letters, numbers and hyphens. The ID local is reserved.')
  if (typeof name !== 'string' || !name.trim() || [...name].length > 80 || /\p{Cc}/u.test(name)) throw Error('Enter a service name of up to 80 characters.')
  if (typeof sshHost !== 'string' || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,252}$/.test(sshHost)) throw Error('Enter a host alias from your SSH configuration.')
  if (gateway !== undefined && (typeof gateway !== 'string' || !/^\/[A-Za-z0-9_./-]{1,1023}$/.test(gateway) || gateway.endsWith('/'))) throw Error('Enter an absolute path to the gateway executable, without spaces or arguments.')
  if (nativeSystem !== undefined || systems !== undefined) {
    if (typeof nativeSystem !== 'string' || !Array.isArray(systems) || !systems.includes(nativeSystem) || !systems.every((system) => linuxSystems.has(system))) {
      throw Error('The execution service returned unsupported Linux systems.')
    }
  }
  return { id, name, sshHost, gateway, nativeSystem, systems }
}

function registry(value: unknown): ExecutionServiceConnection[] {
  if (!Array.isArray(value)) throw Error('Expected an array of execution services.')
  const entries = value.map(validateConnection)
  if (new Set(entries.map((e) => e.id)).size !== entries.length) throw Error('Each execution service must have a distinct ID.')
  return entries
}

export function loadExecutionServices(support: string): ExecutionServiceConnection[] {
  try { return registry(JSON.parse(fs.readFileSync(registryPath(support), 'utf8'))) }
  catch (e) {
    if ((e as NodeJS.ErrnoException).code === 'ENOENT') return []
    throw e
  }
}

export function saveExecutionServices(support: string, value: unknown): ExecutionServiceConnection[] {
  const entries = registry(value)
  fs.mkdirSync(support, { recursive: true, mode: 0o700 })
  const temporary = path.join(support, '.execution-services-' + randomUUID())
  try {
    fs.writeFileSync(temporary, JSON.stringify(entries), { flag: 'wx', mode: 0o600 })
    fs.renameSync(temporary, registryPath(support))
  } finally { fs.rmSync(temporary, { force: true }) }
  return entries
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
  if (typeof size !== 'number' || !Number.isSafeInteger(size) || size < 0) throw Error('Invalid execution service body size.')
  return size
}

class FramedReader {
  private pending = Buffer.alloc(0)
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

function remoteProcess(entry: ExecutionServiceConnection) {
  // OpenSSH invokes the remote shell. The executable is the fixed default
  // name or a validated absolute path. Requests travel on stdin.
  const command = entry.gateway ? "'" + entry.gateway + "'" : 'tsugumi-remote-gateway'
  return spawn('ssh', [
    '-T', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', 'ConnectTimeout=10',
    '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=3',
    '-o', 'ClearAllForwardings=yes', '-o', 'ForwardAgent=no', '--', entry.sshHost,
    command,
  ], { stdio: 'pipe' })
}

function write(stream: Writable, data: Buffer): Promise<void> {
  return new Promise((resolve, reject) => stream.write(data, (error) => error ? reject(error) : resolve()))
}

async function remote(entry: ExecutionServiceConnection, header: Header, input?: FramedReader, socket?: net.Socket): Promise<Header> {
  const child = remoteProcess(entry)
  let stderr = ''
  child.stderr.on('data', (chunk: Buffer) => { stderr = (stderr + chunk.toString('utf8')).slice(-4096) })
  const ended = once(child, 'close').catch(() => {})
  const kill = () => { child.kill() }
  socket?.once('close', kill)
  const timer = socket ? undefined : setTimeout(kill, 60_000)
  void pipeline(Readable.from(sendFrame(header, input)), child.stdin).catch(() => {})
  try {
    const reader = new FramedReader(child.stdout)
    let reply: Header
    try { reply = await reader.header() }
    catch (e) {
      throw Error(socket ? 'The SSH execution gateway did not reply.' : stderr.trim() || (e as Error).message)
    }
    if (socket) {
      await write(socket, frame(reply))
      for await (const chunk of reader.body(bodySize(reply))) await write(socket, chunk)
    }
    return reply
  } finally {
    clearTimeout(timer)
    socket?.removeListener('close', kill)
    kill()
    child.stdin.destroy()
    await ended
  }
}

export async function probeExecutionService(value: unknown): Promise<ExecutionServiceConnection> {
  const entry = validateConnection(value)
  const reply = await remote(entry, { version: 1, operation: 'probe', service: entry.id })
  if (reply.ok !== true) throw Error(typeof reply.error === 'string' ? reply.error : 'The execution service probe failed.')
  const workerContract = 1
  if (reply.worker_contract !== workerContract) {
    const actual = typeof reply.worker_contract === 'number' ? reply.worker_contract : 'unreported'
    throw Error(`SSH gateway worker contract is ${actual}. Expected ${workerContract}. Rebuild the gateway with this Tsugumi revision.`)
  }
  if (typeof reply.native_system !== 'string' || !Array.isArray(reply.systems)) throw Error('The execution service did not report its Linux systems.')
  return validateConnection({ ...entry, nativeSystem: reply.native_system, systems: reply.systems })
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
        const entries = loadExecutionServices(support)
        if (header.operation === 'catalog') {
          const services = entries.map((e) => ({ id: e.id, native_system: e.nativeSystem ?? '', systems: e.systems ?? [], available: !!e.nativeSystem }))
          await write(socket, frame({ version: 1, ok: true, services }))
        } else {
          if (typeof header.operation !== 'string' || !operations.has(header.operation)) throw Error('Unknown execution service operation.')
          const entry = entries.find((e) => e.id === header.service)
          if (!entry) throw Error('The execution service is not registered.')
          delete header.broker_token
          header.project_scope = scope
          forwarding = true
          await remote(entry, header, reader, socket)
        }
      } catch (e) {
        // An SSH interruption can happen after a request reached the gateway.
        // EOF leaves its outcome unconfirmed. Only the gateway can reject it.
        if (!forwarding) await write(socket, frame({ version: 1, ok: false, error: (e as Error).message })).catch(() => {})
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
