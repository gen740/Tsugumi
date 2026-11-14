// The Claude Code plugin's MCP server (stdio, newline-delimited JSON-RPC).
// The host only forwards calls to core's dedicated Agent socket. The tool
// catalog, execution, conversations and access decisions belong to core.
import { spawn } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import readline from 'node:readline'
import { fileURLToPath } from 'node:url'
import { ServerInfo, webDataDir, serverBuild, runningServer, serverRequest } from './paths'

const here = path.dirname(fileURLToPath(import.meta.url))

const INSTRUCTIONS = `Tsugumi builds computations as Cards on a canvas: each Card is a step (Python, C++, bash, ...), \
wires carry typed values between Cards, and a run starts from a trigger Card. Every Card runs in an isolated VM \
with its environment fixed by Nix.
Before working on a project, call tsugumi_open with its folder (create: true makes a new one). It opens the project \
in the Tsugumi web UI, where the user watches each save and run live, and returns the rules for the other tools \
(project_inspect first; saves need the base_rev you inspected). Project content is unavailable until the user \
explicitly grants access. Request access through the Agent API and wait for the user's answer. The plugin \
cannot grant itself access or approve changes to Card permissions. Project RPC tools come only from the shared \
Agent lists: ordinary operations and operations needing approval for each call. You may propose deleting run history, \
scratch or build caches after inspecting their targets and sizes. Cleanup, version checkout, starting versioning, \
keeping a named version and replacing a version summary each need approval in the Tsugumi page. When a call returns \
grant_required, wait for the person to answer there, then retry the same tool with the same arguments. A decline \
changes nothing; continue without repeating that act. Use project_shell for diagnosis and reads (project files are read-only), project_save for edits, \
and Cards for analysis so the user can follow the process.`

const OPEN_TOOL = {
  name: 'tsugumi_open',
  description: 'Opens a Tsugumi project (a folder with tsugumi.json) in the Tsugumi web UI, starting the web UI host when none ' +
    'runs, and makes it the project the other tools work on. The first call also returns the rules for working with them.',
  inputSchema: {
    type: 'object',
    properties: {
      folder: { type: 'string', description: 'The project folder (absolute, or relative to the Claude Code project).' },
      create: { type: 'boolean', description: 'Make a new empty project in this folder when it has no tsugumi.json (the folder must not exist yet).' },
      browser: { type: 'boolean', description: 'Open a browser window when no web UI page is open (default true).' },
    },
    required: ['folder'],
    additionalProperties: false,
  },
}

let current: string | undefined
let rulesSent = false

// ---- The web UI host -------------------------------------------------------------

const buildIdentity = serverBuild(path.join(here, 'server.js'))
const request = serverRequest

let starting: Promise<ServerInfo> | undefined

/** The running host, or one started now (it keeps running after Claude Code ends, like one started by hand). */
async function host(): Promise<ServerInfo> {
  const s = await runningServer(webDataDir(), buildIdentity)
  if (s) return s
  starting ??= (async () => {
    const data = webDataDir()
    fs.mkdirSync(data, { recursive: true })
    const log = fs.openSync(path.join(data, 'server.log'), 'a')
    const child = spawn(process.execPath, [path.join(here, 'server.js'), '--reuse', '--port', process.env.TSUGUMI_WEB_PORT ?? '0'],
      { detached: true, stdio: ['ignore', log, log] })
    let startError: Error | undefined
    child.on('error', e => { startError = e })
    child.unref()
    fs.closeSync(log)
    const deadline = Date.now() + 30_000
    while (Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 200))
      const next = await runningServer(data, buildIdentity)
      // A concurrent command or MCP process may win the shared startup lock.
      if (next) return next
      if (child.exitCode !== null || startError) break
    }
    throw new Error(`the Tsugumi web UI host did not start; see ${path.join(data, 'server.log')}`)
  })().finally(() => { starting = undefined })
  return starting
}

async function api<T>(route: string, body: unknown): Promise<T> {
  const s = await host()
  const r = await request(s, route, body)
  const v = await r.json().catch(() => ({}))
  if (!r.ok) throw new Error((v as { error?: string }).error ?? `${route}: HTTP ${r.status}`)
  return v as T
}

// ---- Tools -----------------------------------------------------------------------

type Result = { content: ({ type: 'text'; text: string } | { type: 'image'; data: string; mimeType: string })[]; isError?: boolean }
const text = (t: string, isError = false): Result => ({ content: [{ type: 'text', text: t }], ...(isError ? { isError } : {}) })

async function open(args: { folder?: unknown; create?: unknown; browser?: unknown }): Promise<Result> {
  if (typeof args.folder !== 'string' || !args.folder.trim()) return text('folder is required', true)
  const dir = path.resolve(process.env.CLAUDE_PROJECT_DIR ?? process.cwd(), args.folder)
  let folder = dir
  if (!fs.existsSync(path.join(dir, 'tsugumi.json'))) {
    if (args.create !== true) return text(`${dir} is not a Tsugumi project (no tsugumi.json). Pass create: true to make a new one there.`, true)
    folder = (await api<{ path: string }>('/api/project/create', { parent: path.dirname(dir), name: path.basename(dir) })).path
  }
  const info = await api<{ path: string }>('/api/project/open', { path: folder })
  current = info.path
  // The host opens the page itself: its key (which answers approvals) is not the plugin's.
  const shown = await api<{ shown: boolean; opened: boolean }>('/api/agent/open', { path: current, browser: args.browser !== false })
  const s = await host()
  let where = `The open web UI page shows it now.`
  if (!shown.shown) {
    where = shown.opened ? `A browser window opens it.`
      : `No web UI page is free to show it (a page on another project keeps that project); the user opens it from the URL the host printed when it started (or call tsugumi_open again without browser: false).`
  }
  let out = `Opened ${current} in the Tsugumi web UI (http://127.0.0.1:${s.port}/). ${where} Opening a project does not grant Agent access to its contents.`
  if (!rulesSent) {
    rulesSent = true
    out += `\n\n${INSTRUCTIONS}`
  }
  return text(out)
}

async function call(name: string, args: unknown): Promise<Result> {
  if (name === OPEN_TOOL.name) return open((args ?? {}) as Record<string, unknown>)
  if (!current && !name.startsWith('agent_')) return text('No Tsugumi project is open: call tsugumi_open with its folder first.', true)
  const r = await api<{ text: string; isError: boolean; images?: { media_type: string; data: string }[] }>('/api/agent/call', { path: current, name, arguments: args ?? {} })
  const result = text(r.text, r.isError)
  for (const i of r.images ?? []) result.content.push({ type: 'image', data: i.data, mimeType: i.media_type })
  return result
}

// ---- JSON-RPC over stdio ---------------------------------------------------------

const VERSIONS = ['2025-06-18', '2025-03-26', '2024-11-05']

function reply(id: unknown, result?: unknown, error?: { code: number; message: string }) {
  process.stdout.write(JSON.stringify(error ? { jsonrpc: '2.0', id, error } : { jsonrpc: '2.0', id, result }) + '\n')
}

async function handle(m: { id?: unknown; method?: string; params?: any }) {
  const isRequest = m.id !== undefined && m.id !== null
  switch (m.method) {
    case 'initialize':
      return reply(m.id, {
        protocolVersion: VERSIONS.includes(m.params?.protocolVersion) ? m.params.protocolVersion : VERSIONS[0],
        capabilities: { tools: {} },
        serverInfo: { name: 'tsugumi', version: '0' },
        instructions: INSTRUCTIONS,
      })
    case 'ping':
      return reply(m.id, {})
    case 'tools/list':
      try {
        return reply(m.id, { tools: [OPEN_TOOL, ...await api<Record<string, unknown>[]>('/api/agent/tools', {})] })
      } catch (e) {
        return reply(m.id, undefined, { code: -32603, message: String((e as Error).message ?? e) })
      }
    case 'tools/call':
      try {
        return reply(m.id, await call(String(m.params?.name), m.params?.arguments))
      } catch (e) {
        return reply(m.id, text(String((e as Error).message ?? e), true))
      }
    default:
      // Notifications (initialized, cancelled) need no answer.
      if (isRequest) reply(m.id, undefined, { code: -32601, message: `method not found: ${m.method}` })
  }
}

readline.createInterface({ input: process.stdin }).on('line', (line) => {
  if (!line.trim()) return
  let m
  try {
    m = JSON.parse(line)
  } catch {
    return reply(null, undefined, { code: -32700, message: 'parse error' })
  }
  void handle(m)
})
