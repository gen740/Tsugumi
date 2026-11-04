// Tsugumi web UI host server: serves the UI, starts one tsugumid per project
// on this host, and hosts the Agent. The browser talks to tsugumid directly
// over its WebSocket RPC; this server only does what a page cannot (files,
// processes, the model API key).
import { ChildProcess, spawn } from 'node:child_process'
import { randomBytes, randomUUID, timingSafeEqual } from 'node:crypto'
import fs from 'node:fs'
import http from 'node:http'
import net from 'node:net'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { chatRoutes, chatStatus } from './chat'
import { EnvironmentManager, EnvState, projectKey } from './env'

const here = path.dirname(fileURLToPath(import.meta.url))
const repo = path.resolve(here, '..', '..')

// ---- Options -------------------------------------------------------------------

const argv = process.argv.slice(2)
const opt = (name: string) => {
  const i = argv.indexOf(name)
  return i >= 0 ? argv[i + 1] : undefined
}
const port = Number(opt('--port') ?? process.env.TSUGUMI_WEB_PORT ?? 8780)
const openBrowser = argv.includes('--open')
const launchProject = argv.filter((a, i) => !a.startsWith('--') && !argv[i - 1]?.startsWith('--')).map((a) => path.resolve(a))
  .find((a) => fs.existsSync(path.join(a, 'tsugumi.json')))

const build = process.env.TSUGUMI_BUILD_DIR ?? path.join(repo, 'build')
const bins = {
  tsugumid: process.env.TSUGUMID ?? path.join(build, 'core', 'tsugumid'),
  worker: process.env.TSUGUMI_WORKER ?? path.join(build, 'sdk', 'tsugumi-worker'),
  sdkDir: process.env.TSUGUMI_SDK_DIR ?? path.join(repo, 'sdk'),
}
const support = process.env.TSUGUMI_WEB_DATA ??
  (process.platform === 'darwin'
    ? path.join(os.homedir(), 'Library', 'Application Support', 'Tsugumi', 'web')
    : path.join(process.env.XDG_DATA_HOME ?? path.join(os.homedir(), '.local', 'share'), 'tsugumi-web'))
fs.mkdirSync(support, { recursive: true })
const demosRoot = path.join(repo, 'demos')
// TSUGUMI_PROJECTS_DIR: where demo copies go instead of ~/Documents/Tsugumi (checks).
const projectsDir = process.env.TSUGUMI_PROJECTS_DIR ?? path.join(os.homedir(), 'Documents', 'Tsugumi')
const publicDir = path.join(here, 'public')

let origins: string[] = []
const envs = new EnvironmentManager(bins, support, () => origins)

// ---- Small JSON stores -----------------------------------------------------------

function readJSON<T>(file: string, fallback: T): T {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'))
  } catch {
    return fallback
  }
}
function writeJSON(file: string, v: unknown) {
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file + '.tmp', JSON.stringify(v, null, 2))
  fs.renameSync(file + '.tmp', file)
}

interface Recent { path: string; name: string; openedAt: number }
const recentsFile = path.join(support, 'recent-projects.json')
const recents = () => readJSON<Recent[]>(recentsFile, []).filter((r) => fs.existsSync(r.path))

function projectName(dir: string) {
  return readJSON<{ name?: string }>(path.join(dir, 'tsugumi.json'), {}).name || path.basename(dir)
}

function remember(dir: string) {
  const list = recents().filter((r) => r.path !== dir)
  list.unshift({ path: dir, name: projectName(dir), openedAt: Date.now() })
  writeJSON(recentsFile, list.slice(0, 30))
}

/** Takes projects off the recent list; the folders themselves stay. */
function forgetRecents(paths: unknown) {
  const gone = new Set(Array.isArray(paths) ? paths.map(String) : [])
  writeJSON(recentsFile, recents().filter((r) => !gone.has(r.path)))
  return { ok: true }
}

// ---- Projects --------------------------------------------------------------------

interface Demo { id: string; title: string; description: string; path: string; tags: string[]; thumbnail: Thumbnail }
interface Thumbnail { cards: { id: string; x: number; y: number; kind: 'link' | 'routing' | 'source' | 'other' }[]; wires: [string, string][] }

/** The top-level Workflow of a demo in miniature: where its Cards are, what
 *  kind they are (from their own definition folders) and how they connect. */
function thumbnail(dir: string): Thumbnail {
  const p = readJSON<any>(path.join(dir, 'tsugumi.json'), {})
  const l = readJSON<any>(path.join(dir, 'layout.json'), {})
  const cards: Thumbnail['cards'] = []
  for (const c of Array.isArray(p?.canvas?.cards) ? p.canvas.cards : []) {
    const id = typeof c?.id === 'string' ? c.id : undefined
    if (!id || id.includes('/') || id.startsWith('.')) continue
    const pos = l?.cards?.[id] ?? {}
    const def = path.join(dir, 'definitions', id)
    const template = readJSON<any>(path.join(def, '_card.json'), {})?.template
    const kind = fs.existsSync(path.join(def, '_link.json')) ? 'link'
      : ['fork', 'branch', 'merge'].includes(template) ? 'routing'
      : template === 'trigger' || template === 'input' ? 'source' : 'other'
    cards.push({ id, x: Number(pos.x) || 0, y: Number(pos.y) || 0, kind })
  }
  const wires: [string, string][] = []
  for (const w of Array.isArray(p?.canvas?.connections) ? p.canvas.connections : []) {
    const a = w?.from?.card, b = w?.to?.card
    if (typeof a === 'string' && typeof b === 'string') wires.push([a, b])
  }
  return { cards, wires }
}

function demos(): Demo[] {
  const idx = readJSON<{ demos?: { id: string; title: string; description?: string; path: string; tags?: string[] }[] }>(path.join(demosRoot, 'index.json'), {})
  return (idx.demos ?? []).map((d) => {
    const dir = path.join(demosRoot, d.path)
    return { id: d.id, title: d.title, description: d.description ?? '', path: dir, tags: Array.isArray(d.tags) ? d.tags.map(String) : [], thumbnail: thumbnail(dir) }
  })
}

/** Copies a bundled demo into a new, editable project (the original stays untouched). */
function copyDemo(id: string): string {
  const demo = demos().find((d) => d.id === id)
  if (!demo) throw new Error(`unknown demo ${id}`)
  fs.mkdirSync(projectsDir, { recursive: true })
  let dest = path.join(projectsDir, id)
  for (let n = 2; fs.existsSync(dest); n++) dest = path.join(projectsDir, `${id}-${n}`)
  fs.cpSync(demo.path, dest, { recursive: true, filter: (src) => !src.includes(`${path.sep}.tsugumi`) })
  return dest
}

const FLOWS = ['topToBottom', 'leftToRight']

/** The nixpkgs this Tsugumi builds with (its flake.lock): new projects are
 *  pinned to it, so a later Tsugumi with another nixpkgs leaves them as they are. */
const nixpkgsPin: string | null = (() => {
  try {
    const locked = JSON.parse(fs.readFileSync(path.join(repo, 'flake.lock'), 'utf8')).nodes.nixpkgs.locked
    return `https://github.com/NixOS/nixpkgs/archive/${locked.rev}.tar.gz#${locked.narHash}`
  } catch {
    return null
  }
})()

/** Creates an empty project; the initial documents are placed before the environment starts. */
function createProject(parent: string, name: string, flow: unknown): string {
  const dir = path.join(parent, name.replace(/[/\\]/g, '-'))
  if (fs.existsSync(dir)) throw new HttpError(409, `${dir} は既に存在します`)
  fs.mkdirSync(dir, { recursive: true })
  const project = { name, pkgsPath: nixpkgsPin ?? 'tsugumi:default', databaseConnections: [], canvas: { cards: [], connections: [] } }
  fs.writeFileSync(path.join(dir, 'tsugumi.json'), JSON.stringify(project, null, 2) + '\n')
  // The direction the new project's Cards are laid out in (the app's default).
  const f = typeof flow === 'string' && FLOWS.includes(flow) ? flow : 'topToBottom'
  fs.writeFileSync(path.join(dir, 'layout.json'), JSON.stringify({ flow: f }) + '\n')
  return dir
}

/** Regular files of a project folder, without hidden files and folders
 *  (.tsugumi, .git, ...) and without symbolic links (e.g. Nix result links). */
function projectFiles(root: string, limit: number): Map<string, Buffer> {
  const out = new Map<string, Buffer>()
  let total = 0
  const walk = (dir: string, rel: string) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      if (e.name.startsWith('.') || e.isSymbolicLink()) continue
      const abs = path.join(dir, e.name), r = rel ? `${rel}/${e.name}` : e.name
      if (e.isDirectory()) walk(abs, r)
      else if (e.isFile()) {
        const d = fs.readFileSync(abs)
        total += d.length
        if (total > limit) throw new HttpError(413, 'プロジェクトが大きすぎます（64 MB まで）。')
        out.set(r, d)
      }
    }
  }
  walk(root, '')
  return out
}

/** Reads another Tsugumi project to take in as a Card in Card. The page puts
 *  it under externals/<name>/ as a copy; the original folder is not linked.
 *  Files that are not UTF-8 text are left out (documents edit text only). */
function importProject(folder: unknown) {
  if (typeof folder !== 'string' || !fs.existsSync(path.join(folder, 'tsugumi.json'))) throw new HttpError(400, 'tsugumi.json のあるフォルダを選んでください。')
  const root = fs.realpathSync(folder)
  const files: Record<string, string> = {}
  const skipped: string[] = []
  const utf8 = new TextDecoder('utf-8', { fatal: true })
  for (const [rel, d] of projectFiles(root, 64 << 20)) {
    try {
      files[rel] = utf8.decode(d)
    } catch {
      skipped.push(rel)
    }
  }
  return { name: path.basename(root), files, skipped }
}

/** A copy of a project's saved documents and sources (what is in its folder,
 *  without hidden folders such as .tsugumi), as a new project folder. */
function duplicateProject(from: string, parent: unknown, name: unknown): string {
  if (typeof parent !== 'string' || typeof name !== 'string' || !name.trim()) throw new HttpError(400, '複製先を指定してください')
  const dest = path.join(parent, name.trim().replace(/[/\\]/g, '-'))
  if (fs.existsSync(dest)) throw new HttpError(409, `${dest} は既に存在します`)
  const files = projectFiles(from, 1 << 30)
  try {
    for (const [rel, d] of files) {
      const f = path.join(dest, rel)
      fs.mkdirSync(path.dirname(f), { recursive: true })
      fs.writeFileSync(f, d)
    }
  } catch (e) {
    fs.rmSync(dest, { recursive: true, force: true })
    throw e
  }
  return dest
}

function listDir(dir: string) {
  const real = fs.realpathSync(dir)
  const entries = fs.readdirSync(real, { withFileTypes: true })
    .filter((e) => e.isDirectory() && !e.name.startsWith('.'))
    .map((e) => ({ name: e.name, project: fs.existsSync(path.join(real, e.name, 'tsugumi.json')) }))
    .sort((a, b) => a.name.localeCompare(b.name))
  return { path: real, parent: path.dirname(real) === real ? null : path.dirname(real), project: fs.existsSync(path.join(real, 'tsugumi.json')), entries }
}

// ---- Events (Server-Sent Events) ------------------------------------------------

const streams = new Set<http.ServerResponse>()
function broadcast(v: unknown) {
  const line = `data: ${JSON.stringify(v)}\n\n`
  for (const s of streams) s.write(line)
}
envs.observe((key, state) => broadcast({ type: 'env', key, state: publicState(state) }))

/** What the page may see of an environment (the token goes only to the same-origin UI). */
function publicState(s: EnvState) {
  if (s.state !== 'ready') return s
  return { state: 'ready', endpoint: s.endpoint, token: s.token, projectId: s.projectId, readyFile: s.readyFile }
}

// ---- Terminals ---------------------------------------------------------------------

/** A shell in a project's folder on this host. No pseudo terminal: the page
 *  sends whole lines and shows what the shell prints (both streams). Only
 *  the same-origin UI can start one (the API header), and they end with the
 *  server. */
interface Terminal { id: string; project: string; proc: ChildProcess }
const terminals = new Map<string, Terminal>()
const MAX_TERMINALS = 8

function startTerminal(project: string) {
  if (terminals.size >= MAX_TERMINALS) throw new HttpError(429, `ターミナルは同時に ${MAX_TERMINALS} 個までです`)
  const id = randomUUID()
  const bash = fs.existsSync('/bin/bash')
  const shell = bash ? '/bin/bash' : '/bin/sh'
  const proc = spawn(shell, bash ? ['--noprofile', '--norc'] : [], {
    cwd: project,
    env: { ...process.env, TERM: 'dumb', PAGER: 'cat', GIT_PAGER: 'cat', TSUGUMI_PROJECT: project },
    stdio: ['pipe', 'pipe', 'pipe'],
    // Its own process group: stopping it ends what it started too.
    detached: true,
  })
  const t: Terminal = { id, project, proc }
  terminals.set(id, t)
  const out = (d: Buffer) => broadcast({ type: 'terminal', id, data: d.toString('utf8') })
  proc.stdout!.on('data', out)
  proc.stderr!.on('data', out)
  proc.on('error', (e) => broadcast({ type: 'terminal', id, data: `\n${e.message}\n` }))
  proc.on('exit', (code, signal) => {
    terminals.delete(id)
    broadcast({ type: 'terminal-exit', id, code, signal })
  })
  return { id, cwd: project, shell }
}

function terminal(id: unknown): Terminal {
  const t = typeof id === 'string' ? terminals.get(id) : undefined
  if (!t) throw new HttpError(404, 'ターミナルは終了しました')
  return t
}

function stopTerminal(t: Terminal, signal: NodeJS.Signals = 'SIGTERM') {
  t.proc.stdin?.end()
  try {
    if (t.proc.pid) process.kill(-t.proc.pid, signal)
  } catch {
    t.proc.kill(signal)
  }
}

// ---- Setup -----------------------------------------------------------------------

let building: { log: string; running: boolean; code?: number } | undefined

function setupStatus() {
  const has = (p: string) => fs.existsSync(p)
  return {
    tsugumid: { path: bins.tsugumid, ok: has(bins.tsugumid) },
    worker: { path: bins.worker, ok: has(bins.worker) },
    sdk: { path: bins.sdkDir, ok: has(path.join(bins.sdkDir, 'nix')) },
    nix: { ok: fs.existsSync('/nix/store') },
    repo,
    building: building ? { running: building.running, code: building.code, log: building.log.slice(-8000) } : null,
  }
}

function startBuild() {
  if (building?.running) return
  building = { log: '', running: true }
  const b = building
  const p = spawn('nix', ['develop', '--command', 'bash', '-c', `cmake -S . -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C "${build}" tsugumid tsugumi-worker`], { cwd: repo })
  const add = (d: Buffer) => (b.log = (b.log + d.toString()).slice(-64 * 1024))
  p.stdout.on('data', add)
  p.stderr.on('data', add)
  p.on('exit', (code) => {
    b.running = false
    b.code = code ?? 1
    broadcast({ type: 'setup' })
  })
  p.on('error', (e) => {
    b.running = false
    b.code = 1
    b.log += `\n${e.message}`
    broadcast({ type: 'setup' })
  })
}

// ---- HTTP ------------------------------------------------------------------------

const MIME: Record<string, string> = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.svg': 'image/svg+xml', '.png': 'image/png', '.map': 'application/json', '.icns': 'image/icns',
}

class HttpError extends Error {
  constructor(readonly status: number, message: string) {
    super(message)
  }
}

async function body(req: http.IncomingMessage): Promise<any> {
  const chunks: Buffer[] = []
  let n = 0
  for await (const c of req) {
    n += (c as Buffer).length
    if (n > 4 << 20) throw new HttpError(413, 'request too large')
    chunks.push(c as Buffer)
  }
  return chunks.length ? JSON.parse(Buffer.concat(chunks).toString('utf8')) : {}
}

function projectDir(p: unknown): string {
  if (typeof p !== 'string' || !fs.existsSync(path.join(p, 'tsugumi.json'))) throw new HttpError(400, 'tsugumi.json を含むフォルダではありません')
  return fs.realpathSync(p)
}

type Handler = (b: any, url: URL) => unknown | Promise<unknown>
const routes: Record<string, Handler> = {
  'GET /api/status': () => ({
    setup: setupStatus(), demos: demos(), recents: recents(), home: os.homedir(), launchProject: launchProject ?? null, nixpkgsPin,
    ...chatStatus(), documents: projectsDir,
  }),
  'POST /api/setup/build': () => {
    startBuild()
    return setupStatus()
  },
  'POST /api/project/open': (b) => {
    const dir = projectDir(b.path)
    remember(dir)
    return { path: dir, key: projectKey(dir).key, name: projectName(dir) }
  },
  'POST /api/project/create': (b) => ({ path: createProject(String(b.parent), String(b.name), b.flow) }),
  'POST /api/project/duplicate': (b) => ({ path: duplicateProject(projectDir(b.path), b.parent, b.name) }),
  'POST /api/recents/forget': (b) => forgetRecents(b.paths),
  // Finder (or the file manager) at a recent project; only folders on the list.
  'POST /api/reveal': (b) => {
    const p = String(b.path)
    if (!recents().some((r) => r.path === p)) throw new HttpError(400, '一覧にないフォルダです')
    const [cmd, args] = process.platform === 'darwin' ? ['open', ['-R', p]] : ['xdg-open', [path.dirname(p)]]
    spawn(cmd, args, { stdio: 'ignore', detached: true }).on('error', () => {}).unref()
    return { ok: true }
  },
  'POST /api/demo/copy': (b) => ({ path: copyDemo(String(b.id)) }),
  'POST /api/env/start': async (b) => publicState(await envs.start(projectDir(b.path))),
  'POST /api/env/stop': async (b) => {
    await envs.stop(projectDir(b.path))
    return { ok: true }
  },
  'GET /api/env/list': () => envs.list().map((e) => ({ key: e.key, project: e.project, name: projectName(e.project), state: publicState(e.state) })),
  'GET /api/fs': (_b, url) => listDir(url.searchParams.get('path') || os.homedir()),
  'POST /api/fs/mkdir': (b) => {
    const dir = path.join(String(b.parent), String(b.name).replace(/[/\\]/g, '-'))
    fs.mkdirSync(dir)
    return listDir(dir)
  },
  'POST /api/import/project': (b) => importProject(b.folder),
  'POST /api/terminal/start': (b) => startTerminal(projectDir(b.path)),
  'POST /api/terminal/input': (b) => {
    const t = terminal(b.id)
    t.proc.stdin?.write(String(b.data ?? ''))
    return { ok: true }
  },
  // Ctrl-C: interrupts the commands the shell is running (not the shell).
  'POST /api/terminal/interrupt': (b) => {
    const pid = terminal(b.id).proc.pid
    if (pid) spawn('pkill', ['-INT', '-P', String(pid)], { stdio: 'ignore' }).on('error', () => {})
    return { ok: true }
  },
  'POST /api/terminal/stop': (b) => {
    stopTerminal(terminal(b.id))
    return { ok: true }
  },
  ...chatRoutes({ support, envs, projectDir, broadcast, httpError: (status, message) => new HttpError(status, message), readJSON, writeJSON, here }),
}

// The page is opened through the URL printed at start, which carries this
// run's key: the server answers it with a cookie only the page's own origin
// sends (HttpOnly, SameSite=Strict) and every /api/ request must carry it.
// Other users of this host and other sites' pages get nothing.
const key = randomBytes(32).toString('hex')
const cookieName = () => `tsugumi_${(server.address() as net.AddressInfo).port}`

function authorized(req: http.IncomingMessage): boolean {
  const want = `${cookieName()}=${key}`
  const got = (req.headers.cookie ?? '').split(';').map((c) => c.trim()).find((c) => c.startsWith(cookieName() + '='))
  return !!got && got.length === want.length && timingSafeEqual(Buffer.from(got), Buffer.from(want))
}

// Every response: no framing, no type sniffing, only this origin's code.
const SECURITY_HEADERS = {
  'content-security-policy': "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; " +
    "font-src 'self' data:; connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'",
  'x-content-type-options': 'nosniff',
  'x-frame-options': 'DENY',
  'referrer-policy': 'no-referrer',
}

function send(res: http.ServerResponse, status: number, v: unknown) {
  res.writeHead(status, { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' })
  res.end(JSON.stringify(v))
}

const server = http.createServer(async (req, res) => {
  for (const [k, v] of Object.entries(SECURITY_HEADERS)) res.setHeader(k, v)
  try {
    await handle(req, res)
  } catch (e) {
    // A malformed request (an undecodable path, a broken body) fails alone.
    if (!res.headersSent) send(res, e instanceof URIError ? 400 : 500, { error: (e as Error).message ?? String(e) })
    else res.end()
  }
})

async function handle(req: http.IncomingMessage, res: http.ServerResponse) {
  const url = new URL(req.url ?? '/', `http://${req.headers.host}`)
  // Loopback names only (no DNS rebinding), and API calls carry a header a
  // cross-origin page cannot send without a CORS preflight we never allow.
  const actualPort = (server.address() as net.AddressInfo).port
  if (![`127.0.0.1:${actualPort}`, `localhost:${actualPort}`].includes(req.headers.host ?? '')) {
    res.writeHead(403).end('forbidden host')
    return
  }
  // The URL printed at start: its key becomes the cookie, and leaves the address bar.
  if (url.searchParams.get('key') === key) {
    url.searchParams.delete('key')
    res.writeHead(302, {
      'set-cookie': `${cookieName()}=${key}; HttpOnly; SameSite=Strict; Path=/`,
      location: url.pathname + url.search,
    })
    return void res.end()
  }
  if (url.pathname.startsWith('/api/') && !authorized(req)) {
    return send(res, 401, { error: 'このページを開く鍵がありません。サーバーが起動時に表示した URL（?key=…）から開き直してください。' })
  }
  if (url.pathname === '/api/events') {
    res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-store', connection: 'keep-alive' })
    res.write(': hello\n\n')
    streams.add(res)
    const ping = setInterval(() => res.write(': ping\n\n'), 15_000)
    req.on('close', () => {
      clearInterval(ping)
      streams.delete(res)
    })
    return
  }
  if (url.pathname.startsWith('/api/')) {
    if (req.method !== 'GET' && req.headers['x-tsugumi'] !== '1') return send(res, 403, { error: 'missing X-Tsugumi header' })
    const h = routes[`${req.method} ${url.pathname}`]
    if (!h) return send(res, 404, { error: 'not found' })
    try {
      send(res, 200, await h(req.method === 'GET' ? {} : await body(req), url))
    } catch (e) {
      send(res, e instanceof HttpError ? e.status : 500, { error: (e as Error).message ?? String(e) })
    }
    return
  }
  let file = path.normalize(path.join(publicDir, decodeURIComponent(url.pathname)))
  if (file !== publicDir && !file.startsWith(publicDir + path.sep)) return void res.writeHead(403).end()
  if (!fs.existsSync(file) || fs.statSync(file).isDirectory()) file = path.join(publicDir, 'index.html')
  res.writeHead(200, { 'content-type': MIME[path.extname(file)] ?? 'application/octet-stream', 'cache-control': 'no-cache' })
  fs.createReadStream(file).pipe(res)
}

server.listen(port, '127.0.0.1', () => {
  const p = (server.address() as net.AddressInfo).port
  origins = [`http://127.0.0.1:${p}`, `http://localhost:${p}`]
  const url = `http://127.0.0.1:${p}/?key=${key}` + (launchProject ? `&project=${encodeURIComponent(launchProject)}` : '')
  console.log(`Tsugumi web UI: ${url}`)
  console.log(`  tsugumid: ${bins.tsugumid}${fs.existsSync(bins.tsugumid) ? '' : ' (missing: build the core first)'}`)
  console.log(`  data:     ${support}`)
  if (openBrowser) spawn(process.platform === 'darwin' ? 'open' : 'xdg-open', [url], { stdio: 'ignore', detached: true }).unref()
})

let stopping = false
async function shutdown() {
  if (stopping) return
  stopping = true
  for (const s of streams) s.end()
  for (const t of terminals.values()) stopTerminal(t, 'SIGKILL')
  server.close()
  // Active runs are interrupted by tsugumid itself; documents stay in the folders.
  await envs.stopAll()
  process.exit(0)
}
process.on('SIGINT', shutdown)
process.on('SIGTERM', shutdown)
