// Browser HTTP/SSE host. agentd owns project environments and Agent execution;
// this process serves the page and adapts authenticated Host requests.
import { spawn, spawnSync } from 'node:child_process'
import { randomBytes, randomUUID, timingSafeEqual } from 'node:crypto'
import fs from 'node:fs'
import http from 'node:http'
import { createRequire } from 'node:module'
import net from 'node:net'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import type { IPty } from 'node-pty'
import { AGENT_KEY_HEADER, AGENT_ROUTES, ServerInfo, webDataDir, serverBuild, runningServer, serverRequest } from './paths'
import { startupLock } from './startup'
import { cancelChats, chatRoutes, chatStatus, hostRequest, observeHost } from './chat'
import { EnvironmentManager, EnvState, projectKey } from './environment'
import type { VMHostOptions, VMTerminal } from './environment'
import { L, language } from './i18n'
import { CoreAgentError } from './coreAgent'
import { hostProjectErrorText } from './hostProjects'
import { uninstall } from './uninstall'
import { terminalEnvironment } from './terminalEnvironment'
import { importProject } from './projectImport'

const here = path.dirname(fileURLToPath(import.meta.url))
const repo = path.resolve(here, '..', '..')
const buildIdentity = serverBuild(fileURLToPath(import.meta.url))

// ---- Options -------------------------------------------------------------------

const argv = process.argv.slice(2)
const opt = (name: string) => {
  const i = argv.indexOf(name)
  return i >= 0 ? argv[i + 1] : undefined
}
// `tsugumi update`: the latest release's install.sh (cli/install.sh) puts the
// newest version in place. The wrappers put their flags before the user's words.
const commandIndex = argv.findIndex((a, i) => !a.startsWith('--') && argv[i - 1] !== '--port')
if (argv[commandIndex] === 'update') process.exit(update())
if (argv[commandIndex] === 'uninstall') process.exit(uninstall(argv.slice(commandIndex + 1)))
if (argv.includes('--version')) {
  console.log(fs.readFileSync(path.join(repo, 'VERSION'), 'utf8').trim())
  process.exit(0)
}

function update(): number {
  // TSUGUMI_INSTALL_BASE (checks, or another place) and TSUGUMI_VERSION go on to install.sh as they are.
  const base = process.env.TSUGUMI_INSTALL_BASE ?? 'https://github.com/gen740/Tsugumi/releases/latest/download'
  let current: string | undefined
  try {
    current = fs.readFileSync(path.join(repo, 'VERSION'), 'utf8').trim()
  } catch {}
  if (current && !process.env.TSUGUMI_INSTALL_BASE && !process.env.TSUGUMI_VERSION) {
    // The latest release, from where GitHub sends releases/latest (no download when this is it).
    const r = spawnSync('curl', ['-fsSI', '-o', '/dev/null', '-w', '%{redirect_url}', 'https://github.com/gen740/Tsugumi/releases/latest'], { encoding: 'utf8' })
    if (/\/tag\/v([^/\s]+)$/.exec(r.stdout?.trim() ?? '')?.[1] === current) {
      console.log(L('tsugumi {0} は最新です。', current))
      return 0
    }
  }
  const r = spawnSync('/bin/sh', ['-c', L('script="$(curl -fsSL "$1/install.sh")" || { echo "$1/install.sh を取得できませんでした。" >&2; exit 1; }\nprintf \'%s\\n\' "$script" | /bin/sh'), 'sh', base], { stdio: 'inherit' })
  if (r.status === 0) console.log(L('動いている tsugumi があれば、Ctrl-C で止めて起動し直すと新しい版で開きます。'))
  return r.status ?? 1
}

const chosenPort = opt('--port') ?? process.env.TSUGUMI_WEB_PORT
const port = Number(chosenPort ?? 8780)
const openBrowser = argv.includes('--open')
// The `tsugumi` command: a server of this user that runs already opens the page instead.
const reuseServer = argv.includes('--reuse')
const launchProject = argv.filter((a, i) => !a.startsWith('--') && argv[i - 1] !== '--port').map((a) => path.resolve(a))
  .find((a) => fs.existsSync(path.join(a, 'tsugumi.json')))

const build = process.env.TSUGUMI_BUILD_DIR ?? path.join(repo, 'build')
const bins = {
  tsugumid: process.env.TSUGUMID ?? path.join(build, 'core', 'tsugumid'),
  worker: process.env.TSUGUMI_WORKER ?? path.join(build, 'workerd', 'tsugumi-worker'),
  sdkDir: process.env.TSUGUMI_SDK_DIR ?? path.join(repo, 'sdk'),
}
const support = webDataDir()
fs.mkdirSync(support, { recursive: true })
// TSUGUMI_PROJECTS_DIR: where demo copies go instead of ~/Documents/Tsugumi (checks).
const projectsDir = process.env.TSUGUMI_PROJECTS_DIR ?? path.join(os.homedir(), 'Documents', 'Tsugumi')
const publicDir = path.join(here, 'public')

let origins: string[] = []
// The Mac's tsugumi command (cli/tsugumi): the desktop app's shared VM and settings.
const vm: VMHostOptions | undefined = process.env.TSUGUMI_VMHOST ? {
  host: process.env.TSUGUMI_VMHOST, image: process.env.TSUGUMI_VM_IMAGE!,
  source: process.env.TSUGUMI_VM_SOURCE!, support: process.env.TSUGUMI_SUPPORT_DIR!,
} : undefined
const desktop = !!vm
const envs = new EnvironmentManager(bins, support, () => origins,
  (method, fields) => hostRequest(method, fields), (handler) => observeHost(handler), vm)

function listDir(dir: string) {
  const real = fs.realpathSync(dir)
  const entries = fs.readdirSync(real, { withFileTypes: true })
    .filter((e) => e.isDirectory() && !e.name.startsWith('.'))
    .map((e) => ({ name: e.name, project: fs.existsSync(path.join(real, e.name, 'tsugumi.json')) }))
    .sort((a, b) => a.name.localeCompare(b.name))
  return { path: real, parent: path.dirname(real) === real ? null : path.dirname(real), project: fs.existsSync(path.join(real, 'tsugumi.json')), entries }
}

// ---- Events (Server-Sent Events) ------------------------------------------------

/** The open pages: browser tabs, each with its own project. */
interface Page {
  id: string
  /** A child workflow's page: it is never given another project. */
  detached: boolean
}
const streams = new Map<http.ServerResponse, Page>()
/** The project each page shows, by its id (none: Home). */
const pageProjects = new Map<string, string>()
function broadcast(v: unknown) {
  const line = `data: ${JSON.stringify(v)}\n\n`
  for (const s of streams.keys()) s.write(line)
}

/** Shows a project without taking another tab's away: a page that shows it
 *  already, or one on Home is told to. False: no page is free for it. */
function showOnPage(project: string): boolean {
  const open = [...streams.entries()]
  if (open.some(([, p]) => pageProjects.get(p.id) === project)) return true
  const home = open.find(([, p]) => !pageProjects.has(p.id) && !p.detached)
  if (!home) return false
  pageProjects.set(home[1].id, project)
  home[0].write(`data: ${JSON.stringify({ type: 'agentOpen', project })}\n\n`)
  return true
}
envs.observe((key, state) => broadcast({ type: 'env', key, state: publicState(state) }))

/** What the page may see of an environment (the token goes only to the same-origin UI). */
function publicState(s: EnvState) {
  if (s.state !== 'ready') return s
  return { state: 'ready', endpoint: s.endpoint, token: s.token, projectId: s.projectId, readyFile: s.readyFile,
    executionScopeUnavailable: s.executionScopeUnavailable }
}

// ---- Terminals ---------------------------------------------------------------------

/** A shell in a project's folder on this host, on a pseudo terminal: the page
 *  sends keys and draws what it prints. Only the same-origin UI can start one
 *  (the API header), and they end with the server. */
interface Terminal { id: string; project: string; proc: IPty | VMTerminal }
const terminals = new Map<string, Terminal>()
const MAX_TERMINALS = 8

/** The native module is loaded when a terminal opens: the server runs without it. */
async function loadPty() {
  try {
    const pty = await import('node-pty')
    // npm drops the helper's executable bit from the package.
    const require = createRequire(import.meta.url)
    const helper = path.join(path.dirname(require.resolve('node-pty/package.json')), 'prebuilds', `${process.platform}-${process.arch}`, 'spawn-helper')
    if (fs.existsSync(helper) && (fs.statSync(helper).mode & 0o111) === 0) fs.chmodSync(helper, 0o755)
    return pty
  } catch (e) {
    throw new HttpError(500, L('ターミナルの部品（node-pty）を読み込めません。web で npm install を実行してください: {0}', (e as Error).message))
  }
}

const terminalSize = (v: unknown, fallback: number) => (Number.isInteger(v) && (v as number) > 0 && (v as number) < 1000 ? (v as number) : fallback)

async function startTerminal(project: string, cols: unknown, rows: unknown) {
  if (terminals.size >= MAX_TERMINALS) throw new HttpError(429, L('ターミナルは同時に {0} 個までです', MAX_TERMINALS))
  const id = randomUUID()
  // In the VM the project's own shell, as in the app's terminal (no node-pty there).
  const env = terminalEnvironment(project)
  const shell = vm ? '/bin/bash' : [env.SHELL, '/bin/bash', '/bin/sh'].find((s) => s && fs.existsSync(s))!
  const proc: IPty | VMTerminal = vm
    ? await envs.terminal(project, terminalSize(cols, 80), terminalSize(rows, 24))
    : (await loadPty()).spawn(shell, ['-l'], {
        name: 'xterm-256color', cwd: project, cols: terminalSize(cols, 80), rows: terminalSize(rows, 24),
        env,
      })
  const t: Terminal = { id, project, proc }
  terminals.set(id, t)
  proc.onData((data) => broadcast({ type: 'terminal', id, data }))
  proc.onExit(({ exitCode, signal }: { exitCode: number; signal?: number }) => {
    terminals.delete(id)
    broadcast({ type: 'terminal-exit', id, code: exitCode, signal })
  })
  return { id, cwd: vm ? '/project' : project, shell }
}

function terminal(id: unknown): Terminal {
  const t = typeof id === 'string' ? terminals.get(id) : undefined
  if (!t) throw new HttpError(404, L('ターミナルは終了しました'))
  return t
}

function stopTerminal(t: Terminal, signal: NodeJS.Signals = 'SIGHUP') {
  try {
    t.proc.kill(signal)
  } catch {}
}

// ---- Setup -----------------------------------------------------------------------

let building: { log: string; running: boolean; code?: number } | undefined

function setupStatus() {
  // In the VM, Tsugumi is built there on its first boot: nothing to do on this host.
  if (vm) {
    const ok = { path: vm.host, ok: true }
    return { tsugumid: ok, worker: ok, sdk: ok, nix: { ok: true }, repo, building: null }
  }
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
  '.wasm': 'application/wasm', '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg', '.json': 'application/json', '.map': 'application/json', '.icns': 'image/icns',
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
  if (typeof p !== 'string' || !fs.existsSync(path.join(p, 'tsugumi.json'))) throw new HttpError(400, L('tsugumi.json を含むフォルダではありません'))
  return fs.realpathSync(p)
}

async function updateExecutionServices(method: string, body: unknown) {
  const result = await hostRequest(method, body)
  for (const environment of envs.list()) {
    if (environment.state.state === 'ready') await envs.start(environment.project)
  }
  return result
}

type Handler = (b: any, url: URL) => unknown | Promise<unknown>
const routes: Record<string, Handler> = {
  'POST /api/layout/arrange': (b) => hostRequest('LayoutArrangeRequest', b),
  'POST /api/layout/untangle': (b) => hostRequest('LayoutUntangleRequest', b),
  'POST /api/layout/validate': (b) => hostRequest('LayoutValidateRequest', b),
  'POST /api/layout/version-canvas': (b) => hostRequest('VersionCanvasRequest', b),
  'GET /api/execution-services': () => hostRequest('ExecutionServicesRequest'),
  'POST /api/execution-services/register': (body) => updateExecutionServices('ExecutionServicePutRequest', body),
  'POST /api/execution-services/remove': (body) => updateExecutionServices('ExecutionServiceRemoveRequest', body),
  'GET /api/status': async () => ({
    setup: setupStatus(), demos: await hostRequest('HostDemoListRequest', { language: language() }),
    recents: await hostRequest('HostRecentListRequest'), home: os.homedir(), launchProject: launchProject ?? null,
    ...await hostRequest('HostProjectDefaultsRequest'),
    // /host/<name> needs bubblewrap: on a Mac, the app's shared VM (core/src/worker_proc.cc).
    hostFolders: process.platform !== 'darwin' || !!vm,
    ...chatStatus(), documents: projectsDir,
  }),
  'POST /api/setup/build': () => {
    startBuild()
    return setupStatus()
  },
  'POST /api/project/open': async (b) => {
    const recent = await hostRequest('HostRecentRememberRequest', { path: b.path })
    return { path: recent.path, key: projectKey(recent.path).key, name: recent.name }
  },
  'POST /api/project/create': (b) => hostRequest('HostProjectCreateRequest', { parent: b.parent, name: b.name, flow: b.flow }),
  'POST /api/project/duplicate': (b) => hostRequest('HostProjectDuplicateRequest', { path: b.path, parent: b.parent, name: b.name }),
  'POST /api/recents/forget': (b) => hostRequest('HostRecentForgetRequest', { paths: b.paths }),
  // Finder (or the file manager) at a recent project; only folders on the list.
  'POST /api/reveal': async (b) => {
    const p = String(b.path)
    if (!(await hostRequest<{ path: string }[]>('HostRecentListRequest')).some((r) => r.path === p)) throw new HttpError(400, L('一覧にないフォルダです'))
    const [cmd, args] = process.platform === 'darwin' ? ['open', ['-R', p]] : ['xdg-open', [path.dirname(p)]]
    spawn(cmd, args, { stdio: 'ignore', detached: true }).on('error', () => {}).unref()
    return { ok: true }
  },
  'POST /api/demo/copy': (b) => hostRequest('HostDemoCopyRequest', { id: b.id, parent: projectsDir, language: language() }),
  // What a page shows now (Home: no project); see showOnPage.
  'POST /api/page': (b) => {
    if (b.project) pageProjects.set(String(b.page), projectDir(b.project))
    else pageProjects.delete(String(b.page))
    return {}
  },
  // A page left the project. Its observation and terminals detach; core keeps running.
  'POST /api/project/close': async (b) => {
    const dir = projectDir(b.path)
    const page = String(b.page)
    pageProjects.delete(page)
    const elsewhere = [...streams.values()].some((p) => !p.detached && p.id !== page && pageProjects.get(p.id) === dir)
    if (elsewhere) return { stopped: false }
    broadcast({ type: 'projectClosed', project: dir })
    cancelChats(dir)
    for (const t of terminals.values()) if (t.project === dir) stopTerminal(t)
    return { stopped: false }
  },
  'POST /api/env/start': async (b) => publicState(await envs.start(projectDir(b.path))),
  'POST /api/env/archive-history': async (b) => {
    const dir = projectDir(b.path)
    const state = await envs.state(dir)
    if (state.state !== 'failed' || !state.historyVersionMismatch) return publicState(state)
    return publicState(await envs.start(dir, true))
  },
  'POST /api/env/stop': async (b) => {
    await envs.stop(projectDir(b.path))
    return { ok: true }
  },
  'GET /api/env/list': () => Promise.all(envs.list().map(async (e) => ({ key: e.key, project: e.project,
    name: (await hostRequest('HostProjectInfoRequest', { path: e.project })).name, state: publicState(e.state) }))),
  // Another run of the tsugumi command: a page opens (its key goes to `open` alone).
  'POST /api/host/identify': () => ({ build: buildIdentity }),
  'POST /api/page/open': (b) => {
    openPage(b.path ? projectDir(b.path) : undefined)
    return { ok: true }
  },
  'GET /api/fs': (_b, url) => listDir(url.searchParams.get('path') || os.homedir()),
  'POST /api/fs/mkdir': (b) => {
    const dir = path.join(String(b.parent), String(b.name).replace(/[/\\]/g, '-'))
    fs.mkdirSync(dir)
    return listDir(dir)
  },
  'POST /api/import/project': (b) => {
    if (typeof b.folder !== 'string') throw new HttpError(400, L('tsugumi.json のあるフォルダを選んでください。'))
    return importProject(b.folder, hostRequest)
  },
  'POST /api/terminal/start': (b) => startTerminal(projectDir(b.path), b.cols, b.rows),
  'POST /api/terminal/input': (b) => {
    terminal(b.id).proc.write(String(b.data ?? ''))
    return {}
  },
  'POST /api/terminal/resize': (b) => {
    const t = terminal(b.id)
    t.proc.resize(terminalSize(b.cols, t.proc.cols), terminalSize(b.rows, t.proc.rows))
    return {}
  },
  'POST /api/terminal/stop': (b) => {
    stopTerminal(terminal(b.id))
    return { ok: true }
  },
  ...chatRoutes({ support, envs, projectDir, broadcast, httpError: (status, message) => new HttpError(status, message), here, listeners: () => streams.size, showOnPage, openPage, desktop }),
}

// The page is opened through the URL printed at start, which carries this
// run's key: the server answers it with a cookie only the page's own origin
// sends (HttpOnly, SameSite=Strict) and every /api/ request must carry it.
// Other users of this host and other sites' pages get nothing.
const key = randomBytes(32).toString('hex')
// Local clients (the Claude Code plugin's MCP server) find this server through
// server.json and get another key, which reaches only AGENT_ROUTES: the page's
// key, which answers approvals, is never written there. A save from Claude
// Code that widens what Cards may do waits for the user on the page.
const agentKey = randomBytes(32).toString('hex')
const serverFile = path.join(support, 'server.json')
const cookieName = () => `tsugumi_${(server.address() as net.AddressInfo).port}`

const sameSecret = (got: string, want: string) =>
  got.length === want.length && timingSafeEqual(Buffer.from(got), Buffer.from(want))

function authorized(req: http.IncomingMessage): boolean {
  const want = `${cookieName()}=${key}`
  const got = (req.headers.cookie ?? '').split(';').map((c) => c.trim()).find((c) => c.startsWith(cookieName() + '='))
  return !!got && sameSecret(got, want)
}

function agentAuthorized(req: http.IncomingMessage, route: string): boolean {
  const got = req.headers[AGENT_KEY_HEADER]
  return typeof got === 'string' && AGENT_ROUTES.has(route) && sameSecret(got, agentKey)
}

/** Opens the page in the default browser (the page's key goes to `open` alone). */
function openPage(project?: string) {
  const p = (server.address() as net.AddressInfo).port
  const url = `http://127.0.0.1:${p}/?key=${key}` + (project ? `&project=${encodeURIComponent(project)}` : '')
  // In WSL (Windows), the browser of Windows: this handler takes a URL as it is, through no shell.
  const [cmd, args] = process.platform === 'darwin' ? ['open', []]
    : process.env.WSL_DISTRO_NAME ? ['rundll32.exe', ['url.dll,FileProtocolHandler']]
    : ['xdg-open', []]
  spawn(cmd, [...args, url], { stdio: 'ignore', detached: true }).on('error', () => {}).unref()
}

// The module viewer (/modules/: a Card's Plotly figure or Vega-Lite chart) is
// framed by the page, always sandboxed (an opaque origin: no cookie, no /api/),
// loads nothing (its script is inline, the page posts it the libraries' code,
// which it evaluates: 'unsafe-eval', also for Vega's expressions) and
// connects nowhere.
const MODULE_HEADERS = {
  'content-security-policy': "sandbox allow-scripts; default-src 'none'; script-src blob: 'unsafe-inline' 'unsafe-eval'; frame-src 'none'; style-src 'unsafe-inline'; " +
    "img-src data: blob:; font-src data:; connect-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'self'",
  'x-frame-options': 'SAMEORIGIN',
}

// Every response: no framing, no type sniffing, only this origin's code.
const SECURITY_HEADERS = {
  'content-security-policy': "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; " +
    "font-src 'self' data:; connect-src 'self' ws://127.0.0.1:* ws://localhost:*; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'",
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
  if (url.pathname.startsWith('/api/') && !authorized(req) && !agentAuthorized(req, `${req.method} ${url.pathname}`)) {
    return send(res, 401, { error: L('このページを開く鍵がありません。サーバーが起動時に表示した URL（?key=…）から開き直してください。') })
  }
  if (url.pathname === '/api/events') {
    res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-store', connection: 'keep-alive' })
    res.write(': hello\n\n')
    const page = { id: url.searchParams.get('page') ?? randomUUID(), detached: url.searchParams.has('detached') }
    streams.set(res, page)
    const ping = setInterval(() => res.write(': ping\n\n'), 15_000)
    req.on('close', () => {
      clearInterval(ping)
      streams.delete(res)
      pageProjects.delete(page.id)
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
      send(res, e instanceof HttpError ? e.status : e instanceof CoreAgentError ? 400 : 500,
        e instanceof CoreAgentError ? { code: e.code, error: hostProjectErrorText(e.code) } : { error: (e as Error).message ?? String(e) })
    }
    return
  }
  let file = path.normalize(path.join(publicDir, decodeURIComponent(url.pathname)))
  if (file !== publicDir && !file.startsWith(publicDir + path.sep)) return void res.writeHead(403).end()
  if (!fs.existsSync(file) || fs.statSync(file).isDirectory()) file = path.join(publicDir, 'index.html')
  else if (file.startsWith(path.join(publicDir, 'modules') + path.sep)) for (const [k, v] of Object.entries(MODULE_HEADERS)) res.setHeader(k, v)
  res.writeHead(200, { 'content-type': MIME[path.extname(file)] ?? 'application/octet-stream', 'cache-control': 'no-cache' })
  fs.createReadStream(file).pipe(res)
}

/** Asks a server of this user that runs already to open a page. */
async function openOnRunningServer(): Promise<boolean> {
  const info = await runningServer(support, buildIdentity)
  if (!info) return false
  if (openBrowser || launchProject) {
    const r = await serverRequest(info, '/api/page/open', { path: launchProject })
    if (!r.ok) throw new Error(`Could not open the project on the running Tsugumi host: HTTP ${r.status}`)
    console.log(L('Tsugumi は動いています（http://127.0.0.1:{0}/）。ブラウザでページを開きました。', info.port))
  }
  return true
}

const releaseStartup = await startupLock(support)
try {
  if (await openOnRunningServer()) {
    releaseStartup()
    process.exit(0)
  }
} catch (e) {
  releaseStartup()
  console.error((e as Error).message)
  process.exit(1)
}

// The usual port keeps the page's own settings (localStorage) across runs; when
// something else has it, any free port (unless one was asked for).
server.on('error', (e: NodeJS.ErrnoException) => {
  if (e.code !== 'EADDRINUSE' || chosenPort !== undefined) throw e
  server.listen(0, '127.0.0.1')
})
server.on('listening', () => {
  const p = (server.address() as net.AddressInfo).port
  origins = [`http://127.0.0.1:${p}`, `http://localhost:${p}`]
  const url = `http://127.0.0.1:${p}/?key=${key}` + (launchProject ? `&project=${encodeURIComponent(launchProject)}` : '')
  console.log(`Tsugumi web UI: ${url}`)
  if (vm) console.log(`  VM:       ${path.join(vm.support, 'vm')}`)
  else console.log(`  tsugumid: ${bins.tsugumid}${fs.existsSync(bins.tsugumid) ? '' : ' (missing: build the core first)'}`)
  console.log(`  data:     ${support}`)
  // For local clients of this user (the Claude Code plugin's MCP server): how
  // to reach this server, with the agent key (not the page's). Readable by this user alone.
  fs.rmSync(serverFile + '.tmp', { force: true })
  fs.writeFileSync(serverFile + '.tmp', JSON.stringify({ build: buildIdentity, port: p, pid: process.pid, agentKey } satisfies ServerInfo), { mode: 0o600 })
  fs.renameSync(serverFile + '.tmp', serverFile)
  releaseStartup()
  if (reuseServer) console.log(L('終了するには Ctrl-C を押します。'))
  if (openBrowser) openPage(launchProject)
})
server.listen(port, '127.0.0.1')

let stopping = false
async function shutdown() {
  if (stopping) return
  stopping = true
  try {
    if (JSON.parse(fs.readFileSync(serverFile, 'utf8')).pid === process.pid) fs.rmSync(serverFile, { force: true })
  } catch {}
  for (const s of streams.keys()) s.end()
  for (const t of terminals.values()) stopTerminal(t, 'SIGKILL')
  server.close()
  // Core and the resident VM host outlive browser and launcher sessions.
  process.exit(0)
}
process.on('SIGINT', shutdown)
process.on('SIGTERM', shutdown)
process.on('SIGHUP', shutdown)
