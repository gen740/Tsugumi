// The Claude Code plugin's MCP server (stdio, newline-delimited JSON-RPC).
// Claude Code works on a Tsugumi project through the web UI host: tsugumi_open
// finds the running host (its server.json) or starts one, and opens the
// project on the page; every other tool is one of the chat Agent's core tools,
// sent to the host (/api/agent/call), which runs it with the same checks and
// approvals as the chat's and shows it on the page (Tsugumi flies to the Card).
// Nothing here talks to tsugumid directly.
import { spawn } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import readline from 'node:readline'
import { fileURLToPath } from 'node:url'
import { BFBS } from '../src/shared/bfbs'
import { externalTools, SYSTEM_PROMPT, ToolCatalog } from './agent'
import { AGENT_KEY_HEADER, ServerInfo, webDataDir } from './paths'

const here = path.dirname(fileURLToPath(import.meta.url))
const catalog = new ToolCatalog(new BFBS(new Uint8Array(fs.readFileSync(path.join(here, 'tsugumi.bfbs')))))

const INSTRUCTIONS = `Tsugumi builds computations as Cards on a canvas: each Card is a step (Python, C++, bash, ...), \
wires carry typed values between Cards, and a run starts from a trigger Card. Every Card runs in an isolated VM \
with its environment fixed by Nix.
Before working on a project, call tsugumi_open with its folder (create: true makes a new one). It opens the project \
in the Tsugumi web UI, where the user watches each save and run live, and returns the rules for the other tools \
(project_inspect first; saves need the base_rev you inspected). When a save widens what Cards may do (network, \
folders), the user approves it in the web UI.`

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

/** What differs for Claude Code from the chat Agent the rules were written for. */
const CLAUDE_CODE_NOTE = `In Claude Code: the tools above are this MCP server's (the names may carry a prefix). \
delegate and attached_folder_read are not available here; use your own subagents and file tools instead. The user \
sees the project in the Tsugumi web UI; say what you changed and ran there, by the Cards' names.`

let current: string | undefined
let rulesSent = false

// ---- The web UI host -------------------------------------------------------------

const serverFile = () => path.join(webDataDir(), 'server.json')

function readServer(): ServerInfo | undefined {
  try {
    return JSON.parse(fs.readFileSync(serverFile(), 'utf8'))
  } catch {
    return undefined
  }
}

async function request(s: ServerInfo, route: string, body: unknown): Promise<Response> {
  return fetch(`http://127.0.0.1:${s.port}${route}`, {
    method: 'POST',
    headers: { 'content-type': 'application/json', 'x-tsugumi': '1', [AGENT_KEY_HEADER]: s.agentKey },
    body: JSON.stringify(body ?? {}),
  })
}

async function alive(s: ServerInfo | undefined): Promise<boolean> {
  if (!s) return false
  try {
    return (await request(s, '/api/agent/tools', {})).ok
  } catch {
    return false
  }
}

let starting: Promise<ServerInfo> | undefined

/** The running host, or one started now (it keeps running after Claude Code ends, like one started by hand). */
async function host(): Promise<ServerInfo> {
  const s = readServer()
  if (await alive(s)) return s!
  starting ??= (async () => {
    const data = webDataDir()
    fs.mkdirSync(data, { recursive: true })
    const log = fs.openSync(path.join(data, 'server.log'), 'a')
    const child = spawn(process.execPath, [path.join(here, 'server.js'), '--port', process.env.TSUGUMI_WEB_PORT ?? '0'],
      { detached: true, stdio: ['ignore', log, log] })
    child.unref()
    fs.closeSync(log)
    const deadline = Date.now() + 30_000
    while (Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 200))
      if (child.exitCode !== null) break
      const next = readServer()
      if (next && next.pid === child.pid && (await alive(next))) return next
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

type Result = { content: { type: 'text'; text: string }[]; isError?: boolean }
const text = (t: string, isError = false): Result => ({ content: [{ type: 'text', text: t }], ...(isError ? { isError } : {}) })

async function open(args: { folder?: unknown; create?: unknown; browser?: unknown }): Promise<Result> {
  if (typeof args.folder !== 'string' || !args.folder.trim()) return text('folder is required', true)
  const dir = path.resolve(process.env.CLAUDE_PROJECT_DIR ?? process.cwd(), args.folder)
  let folder = dir
  if (!fs.existsSync(path.join(dir, 'tsugumi.json'))) {
    if (args.create !== true) return text(`${dir} is not a Tsugumi project (no tsugumi.json). Pass create: true to make a new one there.`, true)
    folder = (await api<{ path: string }>('/api/project/create', { parent: path.dirname(dir), name: path.basename(dir) })).path
  }
  const info = await api<{ path: string; name: string }>('/api/project/open', { path: folder })
  current = info.path
  // The host opens the page itself: its key (which answers approvals) is not the plugin's.
  const shown = await api<{ pages: number; opened: boolean }>('/api/agent/open', { path: current, browser: args.browser !== false })
  const s = await host()
  let where = `The open web UI page shows it now.`
  if (!shown.pages) {
    where = shown.opened ? `A browser window opens it.`
      : `No web UI page is open; the user opens it from the URL the host printed when it started (or call tsugumi_open again without browser: false).`
  }
  let out = `Opened "${info.name}" (${current}) in the Tsugumi web UI (http://127.0.0.1:${s.port}/). ${where}`
  if (!rulesSent) {
    rulesSent = true
    out += `\n\nRules for the Tsugumi tools:\n\n${SYSTEM_PROMPT}\n\n${CLAUDE_CODE_NOTE}`
  }
  return text(out)
}

async function call(name: string, args: unknown): Promise<Result> {
  if (name === OPEN_TOOL.name) return open((args ?? {}) as Record<string, unknown>)
  if (!current) return text('No Tsugumi project is open: call tsugumi_open with its folder first.', true)
  const r = await api<{ text: string; isError: boolean }>('/api/agent/call', { path: current, name, arguments: args ?? {} })
  return text(r.text, r.isError)
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
      return reply(m.id, { tools: [OPEN_TOOL, ...externalTools(catalog).map((t) => ({ name: t.name, description: t.description, inputSchema: t.inputSchema }))] })
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
