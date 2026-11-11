// The Mac's `tsugumi` command: one tsugumid per project in the app's shared
// Linux VM, as in the desktop app, instead of on this host (env.ts). The VM
// belongs to tsugumi-vmhost (macOS/Sources/VMHost), which this server drives
// over its standard input and output. It is another process because only a
// binary signed with the virtualization entitlement can run a VM.
import { ChildProcess, spawn } from 'node:child_process'
import path from 'node:path'
import { createInterface } from 'node:readline'
import { EnvState, Environments, projectKey, writeGrant } from './env'
import { L } from './i18n'

export interface VMHostOptions {
  /** The tsugumi-vmhost binary. */
  host: string
  /** The boot disk and the source the VM builds Tsugumi from. */
  image: string
  source: string
  /** The desktop app's support folder: its VM, store and control folders. */
  support: string
}

/** A terminal in a project's environment, as a pseudo terminal on this host would be. */
export interface VMTerminal {
  cols: number
  rows: number
  write(data: string): void
  resize(cols: number, rows: number): void
  kill(): void
  onData(fn: (data: string) => void): void
  onExit(fn: (e: { exitCode: number; signal?: number }) => void): void
}

// The app's words for the steps of a start (WorkflowTabs.stageText).
const STAGES: Record<string, string> = {
  'installing the VM image': L('VM イメージを展開しています'),
  'creating the Nix store disk': L('Nix store の保存領域を作っています'),
  'booting the VM': L('VM を起動しています'),
  'mounting the Nix store disk': L('Nix store の保存領域を開いています（初回は初期化）'),
  'starting the Nix daemon': L('Nix を準備しています'),
  'configuring the network': L('ネットワークを設定しています'),
  'building Tsugumi in the VM': L('VM の中で Tsugumi を組み立てています（初回と更新後のみ。数分かかることがあります）'),
  'starting the supervisor': L('実行環境の管理プロセスを起動しています'),
  'starting tsugumid': L('このプロジェクトの tsugumid を起動しています'),
  'waiting for tsugumid to become ready': L('tsugumid の応答を待っています'),
}

class HostError extends Error {
  constructor(message: string, readonly recovery: string, readonly log: string) {
    super(message)
  }
}

interface Env { key: string; project: string; state: EnvState; folders: string[] }

export class VMEnvironmentManager implements Environments {
  private proc?: ChildProcess
  private nextId = 1
  private waiting = new Map<number, { resolve: (v: any) => void; reject: (e: Error) => void }>()
  private envs = new Map<string, Env>()
  private listeners = new Set<(key: string, s: EnvState) => void>()
  private terminals = new Map<number, { data: (d: string) => void; exit: (e: { exitCode: number }) => void }>()
  private decoders = new Map<number, TextDecoder>()

  constructor(readonly options: VMHostOptions, private readonly origins: () => string[]) {}

  /** The helper, started on first use and again after it failed. */
  private helper(): ChildProcess {
    if (this.proc && this.proc.exitCode === null && this.proc.signalCode === null) return this.proc
    const { host, image, source, support } = this.options
    // In a session of its own: Ctrl-C reaches this server, which stops the
    // projects first; the helper ends when its standard input does.
    const p = spawn(host, ['--support', support, '--image', image, '--source', source], { stdio: ['pipe', 'pipe', 'inherit'], detached: true })
    this.proc = p
    p.on('error', (e) => this.lost(p, L('tsugumi-vmhost を起動できません: {0}', e.message)))
    p.on('exit', (code, signal) => this.lost(p, L('tsugumi-vmhost が終了しました（{0}）', signal ?? code)))
    p.stdin!.on('error', () => {})
    createInterface({ input: p.stdout! }).on('line', (line) => this.line(line))
    return p
  }

  private lost(p: ChildProcess, message: string) {
    if (this.proc !== p) return
    this.proc = undefined
    for (const w of this.waiting.values()) w.reject(new HostError(message, L('再試行すると起動し直します。文書と実行履歴は消えません。'), ''))
    this.waiting.clear()
    for (const t of this.terminals.values()) t.exit({ exitCode: 1 })
    this.terminals.clear()
    for (const e of this.envs.values()) if (e.state.state === 'ready' || e.state.state === 'starting') this.set(e, { state: 'stopped' })
  }

  private line(line: string) {
    let m: any
    try {
      m = JSON.parse(line)
    } catch {
      return
    }
    if (m.id !== undefined) {
      const w = this.waiting.get(m.id)
      this.waiting.delete(m.id)
      if (m.error !== undefined) w?.reject(new HostError(m.error, m.recovery ?? '', m.log ?? ''))
      else w?.resolve(m.result)
    } else if (m.event === 'progress') {
      const e = this.envs.get(m.key)
      // The steps are known, so the card shows how far along the start is (WorkflowTabs.stageFraction).
      const i = Object.keys(STAGES).indexOf(m.stage)
      const fraction = i < 0 ? undefined : i / Object.keys(STAGES).length
      if (e) this.set(e, { state: 'starting', message: STAGES[m.stage] ?? m.stage, stage: m.stage, fraction })
    } else if (m.event === 'terminal') {
      let d = this.decoders.get(m.session)
      if (!d) this.decoders.set(m.session, (d = new TextDecoder()))
      this.terminals.get(m.session)?.data(d.decode(Buffer.from(m.data, 'base64'), { stream: true }))
    } else if (m.event === 'terminalExit') {
      this.terminals.get(m.session)?.exit({ exitCode: 0 })
      this.terminals.delete(m.session)
      this.decoders.delete(m.session)
    }
  }

  private call<T = any>(op: string, args: Record<string, unknown> = {}): Promise<T> {
    const id = this.nextId++
    const p = this.helper()
    return new Promise((resolve, reject) => {
      this.waiting.set(id, { resolve, reject })
      p.stdin!.write(JSON.stringify({ id, op, ...args }) + '\n')
    })
  }

  observe(fn: (key: string, s: EnvState) => void) {
    this.listeners.add(fn)
    return () => this.listeners.delete(fn)
  }

  private set(e: Env, s: EnvState) {
    e.state = s
    for (const l of this.listeners) l(e.key, s)
  }

  /** The app's control folder of a project (environments/<id>/control). */
  controlDir(key: string) {
    return path.join(this.options.support, 'environments', key, 'control')
  }

  private env(dir: string): Env {
    const { key, real } = projectKey(dir)
    let e = this.envs.get(key)
    if (!e) this.envs.set(key, (e = { key, project: real, state: { state: 'stopped' }, folders: [] }))
    return e
  }

  state(dir: string): EnvState {
    return this.env(dir).state
  }

  /** Starts or reuses the project's tsugumid; the helper knows whether it still runs. */
  async start(dir: string): Promise<EnvState> {
    const e = this.env(dir)
    if (e.state.state !== 'ready') this.set(e, { state: 'starting', message: L('実行環境を起動しています') })
    try {
      const r = await this.call('start', { path: e.project, origins: this.origins() })
      const s: EnvState = { state: 'ready', endpoint: r.endpoint, token: r.token, projectId: r.projectId, readyFile: r.readyFile }
      this.set(e, s)
      return s
    } catch (error) {
      const s = failure(error)
      this.set(e, s)
      return s
    }
  }

  async stop(dir: string): Promise<void> {
    const e = this.env(dir)
    if (this.proc) await this.call('stop', { path: e.project }).catch(() => {})
    this.set(e, { state: 'stopped' })
  }

  stopAll(): Promise<void[]> {
    return Promise.all([...this.envs.values()].filter((e) => e.state.state !== 'stopped').map((e) => this.stop(e.project)))
  }

  /** Ends the helper: it stops the VM, and this waits for that. */
  shutdown(): Promise<void> {
    const p = this.proc
    if (!p) return Promise.resolve()
    return new Promise((resolve) => {
      const t = setTimeout(() => p.kill('SIGKILL'), 60_000)
      p.once('exit', () => {
        clearTimeout(t)
        resolve()
      })
      p.stdin!.end()
    })
  }

  /** As the app's chat: a folder is shared when the environment starts, so a
   *  new one starts it again (the page reconnects), and one taken off goes then. */
  async attachFolders(dir: string, _key: string, folders: string[]): Promise<void> {
    const e = this.env(dir)
    const added = folders.some((f) => !e.folders.includes(f))
    e.folders = folders
    await this.call('folders', { path: e.project, folders })
    if (added && e.state.state === 'ready') {
      await this.stop(e.project)
      await this.start(e.project)
    }
  }

  grantImport(dir: string, source: string): string {
    const control = this.controlDir(this.env(dir).key)
    // The folder as tsugumid sees it in the VM.
    return writeGrant(control, source, (grant) => `/control/grants/${grant}`)
  }

  list(): { key: string; project: string; state: EnvState }[] {
    return [...this.envs.values()].map((e) => ({ key: e.key, project: e.project, state: e.state }))
  }

  /** A shell in the project's environment in the VM (its uid and namespaces, in /project). */
  async terminal(dir: string, cols: number, rows: number): Promise<VMTerminal> {
    const s = await this.start(dir)
    if (s.state !== 'ready') throw new Error(s.state === 'failed' ? s.message : L('実行環境が起動していません'))
    const { session } = await this.call<{ session: number }>('terminal', { path: this.env(dir).project, cols, rows })
    const handlers: { data: (d: string) => void; exit: (e: { exitCode: number }) => void } = { data: () => {}, exit: () => {} }
    this.terminals.set(session, handlers)
    const t: VMTerminal = {
      cols, rows,
      write: (data) => void this.call('input', { session, data: Buffer.from(data).toString('base64') }).catch(() => {}),
      resize: (c, r) => {
        t.cols = c
        t.rows = r
        void this.call('resize', { session, cols: c, rows: r }).catch(() => {})
      },
      kill: () => void this.call('closeTerminal', { session }).catch(() => {}),
      onData: (fn) => void (handlers.data = fn),
      onExit: (fn) => void (handlers.exit = fn),
    }
    return t
  }

  // ---- The desktop app's settings -------------------------------------------

  prefs(keys: string[]): Promise<Record<string, unknown>> {
    return this.call('prefs', { keys })
  }

  setPrefs(values: Record<string, unknown>): Promise<void> {
    return this.call('setPrefs', { values })
  }
}

function failure(error: unknown): EnvState {
  const e = error as HostError
  // One VM per support folder (SharedVM.start): the app has it, or another server.
  if (e.message?.includes('another Tsugumi is using')) {
    return { state: 'failed', message: L('デスクトップ版の Tsugumi か別の tsugumi コマンドが共通 VM を使っています。'),
      recovery: L('VM は同時に 1 つのアプリしか使えません。もう一方を終了してから再試行してください。文書と実行履歴は消えません。'), log: '' }
  }
  return { state: 'failed', message: e.message ?? String(e), recovery: e.recovery ?? L('再試行してください。'), log: e.log ?? '' }
}
