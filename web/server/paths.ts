// Where the web UI host keeps its data, shared by the server and the Claude
// Code plugin's MCP server (which finds a running server there).
import os from 'node:os'
import path from 'node:path'
import fs from 'node:fs'
import { createHash } from 'node:crypto'

export function webDataDir(): string {
  return process.env.TSUGUMI_WEB_DATA ??
    (process.platform === 'darwin'
      ? path.join(os.homedir(), 'Library', 'Application Support', 'Tsugumi', 'web')
      : path.join(process.env.XDG_DATA_HOME ?? path.join(os.homedir(), '.local', 'share'), 'tsugumi-web'))
}

/** What a running server writes for local clients of this user (mode 0600):
 *  the Claude Code plugin's MCP server. Its key is not the page's: it reaches
 *  only the routes Claude Code works through (AGENT_ROUTES below), never
 *  the answers to approvals, the event stream, terminals or files. */
export interface ServerInfo {
  build: string
  port: number
  pid: number
  /** Sent in the X-Tsugumi-Agent header. */
  agentKey: string
}

/** Immutable runtime location and bundle contents distinguish installations
 * and rebuilds, including two builds with the same release version. */
export function serverBuild(script: string): string {
  const hash = createHash('sha256').update(fs.realpathSync(script)).update('\0').update(fs.readFileSync(script))
  const repo = path.resolve(path.dirname(script), '..', '..')
  // Nix can reuse the Web output while changing the native runtime injected
  // by the command's wrapper. The plugin and its child inherit the same paths.
  for (const [name, fallback] of [
    ['TSUGUMID', 'core/tsugumid'], ['TSUGUMI_AGENTD', 'core/tsugumi-agentd'],
    ['TSUGUMI_WORKER', 'workerd/tsugumi-worker'], ['TSUGUMI_VMHOST', ''],
  ]) {
    const binary = process.env[name] ?? (fallback ? path.join(process.env.TSUGUMI_BUILD_DIR ?? path.join(repo, 'build'), fallback) : '')
    hash.update('\0' + binary)
    if (binary && fs.existsSync(binary)) hash.update(fs.readFileSync(binary))
  }
  return hash.digest('hex')
}

export function readServer(data: string): ServerInfo | undefined {
  try {
    const s = JSON.parse(fs.readFileSync(path.join(data, 'server.json'), 'utf8'))
    if (!s || typeof s.build !== 'string' || !/^[a-f0-9]{64}$/.test(s.build) ||
        !Number.isInteger(s.port) || s.port < 1 || s.port > 65535 ||
        !Number.isInteger(s.pid) || s.pid < 1 || typeof s.agentKey !== 'string' || !/^[a-f0-9]{64}$/.test(s.agentKey)) {
      throw new Error('Invalid server.json; stop the running Tsugumi host and remove this file before restarting.')
    }
    return s
  } catch (e) {
    if ((e as NodeJS.ErrnoException).code === 'ENOENT') return undefined
    throw e
  }
}

export async function serverRequest(s: ServerInfo, route: string, body: unknown = {}): Promise<Response> {
  return fetch(`http://127.0.0.1:${s.port}${route}`, {
    method: 'POST', signal: AbortSignal.timeout(5000),
    headers: { 'content-type': 'application/json', 'x-tsugumi': '1', [AGENT_KEY_HEADER]: s.agentKey },
    body: JSON.stringify(body),
  })
}

export async function runningServer(data: string, build: string): Promise<ServerInfo | undefined> {
  const s = readServer(data)
  if (!s) return undefined
  try { process.kill(s.pid, 0) } catch (e) {
    if ((e as NodeJS.ErrnoException).code === 'ESRCH') return undefined
    throw e
  }
  if (s.build !== build) throw new Error(`A different Tsugumi build is running (PID ${s.pid}, http://127.0.0.1:${s.port}/). Stop that host and restart tsugumi to use this build.`)
  // A live but unresponsive host must not lead to a duplicate host.
  const r = await serverRequest(s, '/api/host/identify')
  if (!r.ok || (await r.json() as { build?: string }).build !== build) {
    throw new Error(`The running Tsugumi host (PID ${s.pid}) could not be verified. Stop it before restarting.`)
  }
  return s
}

/** The header that carries ServerInfo.agentKey. */
export const AGENT_KEY_HEADER = 'x-tsugumi-agent'

/** The routes the agent key reaches: what the plugin's MCP server calls, and
 *  another run of the tsugumi command asking this server to open a page. */
export const AGENT_ROUTES: ReadonlySet<string> = new Set([
  'POST /api/host/identify',
  'POST /api/project/open',
  'POST /api/project/create',
  'POST /api/agent/tools',
  'POST /api/agent/open',
  'POST /api/agent/call',
  'POST /api/page/open',
])
