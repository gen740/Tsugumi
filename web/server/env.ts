// Runs one tsugumid per project directly on this host (no VM): the web UI
// talks to it over its WebSocket endpoint, the Agent over its Unix socket.
import { ChildProcess, spawn } from 'node:child_process'
import { createHash, randomUUID } from 'node:crypto'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { L } from './i18n'

export interface Binaries {
  tsugumid: string
  worker: string
  sdkDir: string
}

export type EnvState =
  | { state: 'stopped' }
  // stage: the step of a start in the VM (vm.ts), and how far along it is.
  | { state: 'starting'; message: string; stage?: string; fraction?: number }
  // unix: the Agent's raw socket; a tsugumid in the VM (vm.ts) has only the endpoint.
  | { state: 'ready'; endpoint: string; token: string; projectId: string; unix?: string; pid?: number; readyFile: string }
  | { state: 'failed'; message: string; recovery: string; log: string }

/** Where the server runs each project's tsugumid: on this host
 *  (EnvironmentManager) or in the Mac's shared VM (VMEnvironmentManager, vm.ts). */
export interface Environments {
  observe(fn: (key: string, s: EnvState) => void): () => boolean
  controlDir(key: string): string
  state(dir: string): EnvState
  start(dir: string): Promise<EnvState>
  stop(dir: string): Promise<void>
  stopAll(): Promise<void[]>
  /** The folders a project's chats may read now. */
  attachFolders(dir: string, key: string, folders: string[]): void | Promise<void>
  grantImport(dir: string, source: string): string
  list(): { key: string; project: string; state: EnvState }[]
}

/** Copies what ProjectImport reads of a project into the control folder and
 *  lists it in grants.json, under the path tsugumid will read it from. */
export function writeGrant(control: string, source: string, seenAs: (grant: string, dest: string) => string): string {
  const grant = randomUUID()
  const dest = path.join(control, 'grants', grant)
  fs.mkdirSync(dest, { recursive: true })
  for (const item of ['tsugumi.json', 'layout.json', 'definitions']) {
    const src = path.join(source, item)
    if (fs.existsSync(src)) fs.cpSync(src, path.join(dest, item), { recursive: true })
  }
  const grantsFile = path.join(control, 'grants.json')
  let grants: { grants: Record<string, string> } = { grants: {} }
  try {
    grants = JSON.parse(fs.readFileSync(grantsFile, 'utf8'))
  } catch {}
  grants.grants[grant] = seenAs(grant, dest)
  fs.writeFileSync(grantsFile + '.tmp', JSON.stringify(grants))
  fs.renameSync(grantsFile + '.tmp', grantsFile)
  return grant
}

interface Env {
  key: string
  project: string
  control: string
  sockDir: string
  proc?: ChildProcess
  log: string
  state: EnvState
  starting?: Promise<EnvState>
  stopping?: Promise<void>
}

/** Stable identity of a project folder: its real path (aliases share it). */
export function projectKey(dir: string): { key: string; real: string } {
  const real = fs.realpathSync(dir)
  const st = fs.statSync(real)
  return { key: createHash('sha256').update(`${st.dev}:${st.ino}`).digest('hex').slice(0, 16), real }
}

export class EnvironmentManager implements Environments {
  private envs = new Map<string, Env>()
  private listeners = new Set<(key: string, s: EnvState) => void>()

  constructor(
    readonly bins: Binaries,
    readonly support: string,
    private readonly origins: () => string[],
  ) {}

  observe(fn: (key: string, s: EnvState) => void) {
    this.listeners.add(fn)
    return () => this.listeners.delete(fn)
  }

  private set(e: Env, s: EnvState) {
    e.state = s
    for (const l of this.listeners) l(e.key, s)
  }

  controlDir(key: string) {
    return path.join(this.support, 'env', key, 'control')
  }

  private env(dir: string): Env {
    const { key, real } = projectKey(dir)
    let e = this.envs.get(key)
    if (!e) {
      const control = this.controlDir(key)
      // Unix socket paths are short-limited; keep them in the temp directory.
      const sockDir = path.join(os.tmpdir(), `tsgw-${key.slice(0, 10)}`)
      e = { key, project: real, control, sockDir, log: '', state: { state: 'stopped' } }
      this.envs.set(key, e)
    }
    return e
  }

  state(dir: string): EnvState {
    return this.env(dir).state
  }

  /** Starts (or reuses) the project's tsugumid and waits for server.ready. */
  start(dir: string): Promise<EnvState> {
    const e = this.env(dir)
    // Opened again while it stops: a new one starts once the old one has gone.
    if (e.stopping) return e.stopping.then(() => this.start(dir))
    fs.mkdirSync(e.control, { recursive: true, mode: 0o700 })
    const origins = path.join(e.control, 'origins.json')
    const temporary = origins + '.' + randomUUID()
    fs.writeFileSync(temporary, JSON.stringify({ origins: this.origins() }), { mode: 0o600 })
    fs.renameSync(temporary, origins)
    const running = this.readReady(e)
    if (running) { this.set(e, running); return Promise.resolve(running) }
    if (e.starting) return e.starting
    e.starting = this.launch(e).finally(() => (e.starting = undefined))
    return e.starting
  }

  private readReady(e: Env): EnvState | undefined {
    const file = path.join(e.control, 'server.ready')
    try {
      const r = JSON.parse(fs.readFileSync(file, 'utf8'))
      if (r.protocol !== 1 || !r.pid || !r.token || !r.project_id || !r.unix_socket || !fs.statSync(r.unix_socket).isSocket()) return undefined
      process.kill(r.pid, 0)
      return { state: 'ready', endpoint: r.endpoint, token: r.token, projectId: r.project_id,
        unix: r.unix_socket, pid: r.pid, readyFile: file }
    } catch { return undefined }
  }

  private async launch(e: Env): Promise<EnvState> {
    for (const [what, p] of [['tsugumid', this.bins.tsugumid], ['tsugumi-worker', this.bins.worker]] as const) {
      if (!fs.existsSync(p)) {
        const s: EnvState = { state: 'failed', message: L('{0} が見つかりません: {1}', what, p), recovery: L('リポジトリで core をビルドしてください（README の「Web UI」）。'), log: '' }
        this.set(e, s)
        return s
      }
    }
    this.set(e, { state: 'starting', message: L('tsugumid を起動しています') })
    fs.mkdirSync(e.control, { recursive: true, mode: 0o700 })
    fs.chmodSync(e.control, 0o700)
    fs.mkdirSync(e.sockDir, { recursive: true, mode: 0o700 })
    // In the shared temp directory another user could have made it first.
    const st = fs.lstatSync(e.sockDir)
    if (!st.isDirectory() || st.uid !== process.getuid!() || (st.mode & 0o077) !== 0) {
      const s: EnvState = { state: 'failed', message: L('ソケットのフォルダ {0} が自分専用ではありません（所有者かアクセス権が違います）', e.sockDir),
        recovery: L('このフォルダを削除してから、もう一度開いてください。'), log: '' }
      this.set(e, s)
      return s
    }
    const grants = path.join(e.control, 'grants.json')
    if (!fs.existsSync(grants)) fs.writeFileSync(grants, '{"grants":{}}')
    const ready = path.join(e.control, 'server.ready')
    fs.rmSync(ready, { force: true })
    const unix = path.join(e.sockDir, 's')
    fs.rmSync(unix, { force: true })
    const args = ['--project', e.project, '--listen', '127.0.0.1:0', '--unix', unix, '--control-dir', e.control,
      '--worker', this.bins.worker, '--sdk-dir', this.bins.sdkDir, '--cpus', String(Math.max(1, os.cpus().length))]
    for (const o of this.origins()) args.push('--allow-origin', o)
    e.log = ''
    const logFile = path.join(e.control, 'server.log')
    const log = fs.openSync(logFile, 'a', 0o600)
    const proc = spawn(this.bins.tsugumid, args, { detached: true, stdio: ['ignore', log, log] })
    fs.closeSync(log)
    proc.unref()
    e.proc = proc
    let startupError: Error | undefined
    proc.on('error', (error) => { startupError = error })
    proc.on('exit', (code, sig) => {
      if (e.proc !== proc) return
      e.proc = undefined
      if (e.state.state === 'ready' || e.state.state === 'starting') {
        this.set(e, code === 0 || sig === 'SIGTERM'
          ? { state: 'stopped' }
          : { state: 'failed', message: L('tsugumid が終了しました（{0}）', sig ?? code), recovery: L('再試行すると起動し直します。未保存の変更は画面に保持しています。'), log: e.log })
      }
    })
    const deadline = Date.now() + 60_000
    while (Date.now() < deadline) {
      if (startupError || proc.exitCode !== null || proc.signalCode !== null) {
        e.log = startupError?.message ?? fs.readFileSync(logFile, 'utf8').slice(-64 * 1024)
        const s: EnvState = {
          state: 'failed',
          message: L('tsugumid が起動中に終了しました'),
          recovery: e.log.includes('already owns') ? L('同じプロジェクトを別の tsugumid（macOS アプリなど）が使っています。そちらを閉じてから再試行してください。') : L('ログを確認してください。'),
          log: e.log,
        }
        this.set(e, s)
        return s
      }
      if (fs.existsSync(ready)) {
        const r = JSON.parse(fs.readFileSync(ready, 'utf8'))
        const s: EnvState = { state: 'ready', endpoint: r.endpoint, token: r.token, projectId: r.project_id, unix: r.unix_socket, pid: r.pid, readyFile: ready }
        this.set(e, s)
        return s
      }
      await new Promise((r) => setTimeout(r, 50))
    }
    proc.kill('SIGKILL')
    const s: EnvState = { state: 'failed', message: L('tsugumid が 60 秒以内に準備できませんでした'), recovery: L('再試行してください。'), log: e.log }
    this.set(e, s)
    return s
  }

  /** Stops the project's tsugumid; active runs are interrupted by it. */
  async stop(dir: string): Promise<void> {
    const e = this.env(dir)
    const p = e.proc
    if (!p) {
      const ready = this.readReady(e)
      if (ready?.state === 'ready' && ready.pid) {
        const pid = ready.pid
        process.kill(pid, 'SIGTERM')
        const deadline = Date.now() + 20_000
        while (this.readReady(e) && Date.now() < deadline) await new Promise((resolve) => setTimeout(resolve, 50))
        if (this.readReady(e)) process.kill(pid, 'SIGKILL')
      }
      this.set(e, { state: 'stopped' })
      return
    }
    e.stopping ??= new Promise<void>((resolve) => {
      const t = setTimeout(() => p.kill('SIGKILL'), 20_000)
      p.once('exit', () => {
        clearTimeout(t)
        resolve()
      })
      p.kill('SIGTERM')
    }).finally(() => (e.stopping = undefined))
    await e.stopping
    this.set(e, { state: 'stopped' })
  }

  stopAll(): Promise<void[]> {
    return Promise.all([...this.envs.values()].map((e) => this.stop(e.project)))
  }

  /** Registers a read-only snapshot of an external project for ProjectImport.
   *  The copy is fixed at this moment; core only reads granted copies. */
  grantImport(dir: string, source: string): string {
    return writeGrant(this.env(dir).control, source, (_grant, dest) => dest)
  }

  /** tsugumid on this host reads only the folders listed in its control
   *  folder (attached.json), written here alone. */
  attachFolders(_dir: string, key: string, folders: string[]) {
    const control = this.controlDir(key)
    fs.mkdirSync(control, { recursive: true, mode: 0o700 })
    const file = path.join(control, 'attached.json')
    fs.writeFileSync(file + '.tmp', JSON.stringify({ folders }), { mode: 0o600 })
    fs.renameSync(file + '.tmp', file)
  }

  list(): { key: string; project: string; state: EnvState }[] {
    return [...this.envs.values()].map((e) => ({ key: e.key, project: e.project, state: e.state }))
  }
}
