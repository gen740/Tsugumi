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
  const hash = createHash('sha256').update(fs.realpathSync(script)).update('\0').update(`${fs.statSync(script).size}:${fs.statSync(script).mtimeMs}`)
  const repo = runtimeRoot(path.dirname(script))
  // Nix can reuse the Web output while changing the native runtime injected
  // by the command's wrapper. The plugin and its child inherit the same paths.
  for (const [name, fallback] of [
    ['TSUGUMID', 'tsugumid/tsugumid'], ['TSUGUMI_HOSTD', 'hostd/tsugumi-hostd'],
    ['TSUGUMI_WORKER', 'workerd/tsugumi-worker'], ['TSUGUMI_VMHOST', ''],
  ]) {
    const binary = process.env[name] ?? (fallback ? path.join(process.env.TSUGUMI_BUILD_DIR ?? path.join(repo, 'build'), fallback) : '')
    hash.update('\0' + binary)
    if (binary && fs.existsSync(binary)) { const stat = fs.statSync(binary); hash.update(`${stat.size}:${stat.mtimeMs}`) }
  }
  return hash.digest('hex')
}

export function readServer(data: string): ServerInfo | undefined {
  try {
    const s = JSON.parse(fs.readFileSync(path.join(data, 'server.json'), 'utf8'))
    return s
  } catch (e) {
    if ((e as NodeJS.ErrnoException).code === 'ENOENT') return undefined
    throw e
  }
}

export async function serverRequest(s: ServerInfo, route: string, body: unknown = {}, signal?: AbortSignal): Promise<Response> {
  return fetch(`http://127.0.0.1:${s.port}${route}`, {
    method: 'POST', signal,
    headers: { 'content-type': 'application/json', [AGENT_KEY_HEADER]: s.agentKey },
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
  return s
}

/** The header that carries ServerInfo.agentKey. */
export const AGENT_KEY_HEADER = 'x-tsugumi-agent'

/** The routes the agent key reaches: what the plugin's MCP server calls, and
 *  another run of the tsugumi command asking this server to open a page. */
export const AGENT_ROUTES: ReadonlySet<string> = new Set([
  'POST /api/project/open',
  'POST /api/project/create',
  'POST /api/agent/tools',
  'POST /api/agent/open',
  'POST /api/agent/call',
  'POST /api/page/open',
])

/** Checkout and distribution roots are different, supplied by the package wrapper. */
export function runtimeRoot(directory: string): string {
  return process.env.TSUGUMI_REPO ?? path.resolve(directory, '..', '..', '..')
}
