// Presentation adapter for core-owned project environments.
import { createHash, randomUUID } from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { L } from './i18n'
import { HistoryVersionMismatch, historyVersionMismatch } from '../src/shared/historyVersion'
import { EnvironmentPhase } from '../src/gen/tsugumi/host/environment-phase'

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

export interface VMHostOptions { host: string; image: string; source: string; support: string }
export interface VMTerminal {
  cols: number; rows: number; write(data: string): void; resize(cols: number, rows: number): void; kill(): void;
  onData(fn: (data: string) => void): void; onExit(fn: (event: { exitCode: number }) => void): void;
}
export type HostEvent = { type: string; value: any }
type Request = <T = any>(method: string, fields?: unknown) => Promise<T>
type Observe = (fn: (event: HostEvent) => void) => () => boolean
export interface Binaries {
  tsugumid: string
  worker: string
  sdkDir: string
}

export type EnvState =
  | { state: 'stopped' }
  // stage: the step of a start in the VM, and how far along it is.
  | { state: 'starting'; message: string; stage?: string; fraction?: number }
  // unix: the Agent's raw socket; a tsugumid in the VM has only the endpoint.
  | { state: 'ready'; endpoint: string; token: string; projectId: string; unix?: string; pid?: number; readyFile: string; executionScopeUnavailable?: boolean }
  | { state: 'failed'; message: string; recovery: string; log: string; historyVersionMismatch?: HistoryVersionMismatch }

/** Where the server runs each project's tsugumid: on this host
 *  through the human-authenticated Host contract. */
export interface Environments {
  observe(fn: (key: string, s: EnvState) => void): () => boolean
  controlDir(key: string): string
  readonly agentArguments?: string[]
  state(dir: string): Promise<EnvState>
  start(dir: string, archiveIncompatibleHistory?: boolean): Promise<EnvState>
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

/** Stable identity of a project folder. Aliases and renames share its inode. */
export function projectKey(dir: string): { key: string; real: string } {
  const real = fs.realpathSync(dir)
  const st = fs.statSync(real, { bigint: true })
  const inode = `${st.dev}:${st.ino}`
  return { key: createHash('sha256').update(inode).digest('hex').slice(0, 16), real }
}

interface Env { key: string; project: string; state: EnvState; folders: string[]; control?: string }

export class EnvironmentManager implements Environments {
  private envs = new Map<string, Env>()
  private listeners = new Set<(key: string, state: EnvState) => void>()
  private observing = false
  private terminals = new Map<number, { data: (value: string) => void; exit: (value: { exitCode: number }) => void; decoder: TextDecoder }>()
  private terminalOutput = new Map<number, { data: number[]; closed: boolean }>()
  readonly agentArguments: string[]

  constructor(readonly bins: Binaries, readonly support: string, private origins: () => string[],
    private request: Request, private observeHost: Observe, readonly vm?: VMHostOptions) {
    this.agentArguments = vm
      ? ['--vmhost', vm.host, '--vm-support', vm.support, '--vm-image', vm.image, '--vm-source', vm.source]
      : ['--tsugumid', bins.tsugumid, '--worker', bins.worker, '--sdk-dir', bins.sdkDir,
         '--environment-dir', path.join(support, 'env')]
  }
  private env(dir: string): Env {
    const { key, real } = projectKey(dir)
    let entry = this.envs.get(key)
    if (!entry) this.envs.set(key, entry = { key, project: real, state: { state: 'stopped' }, folders: [] })
    entry.project = real
    if (!this.observing) {
      this.observing = true
      this.observeHost((event) => this.event(event))
    }
    return entry
  }
  observe(fn: (key: string, state: EnvState) => void) {
    this.listeners.add(fn)
    return () => this.listeners.delete(fn)
  }
  private show(entry: Env, state: EnvState) {
    entry.state = state
    for (const listener of this.listeners) listener(entry.key, state)
    return state
  }
  private event(event: HostEvent) {
    if (event.type === 'HostTerminalData') {
      const value = event.value
      const terminal = this.terminals.get(value.session)
      if (terminal) {
        if (value.data?.length) terminal.data(terminal.decoder.decode(Uint8Array.from(value.data), { stream: true }))
        if (value.closed) { terminal.exit({ exitCode: 0 }); this.terminals.delete(value.session) }
      } else if (this.terminalOutput.size < 16) {
        const pending = this.terminalOutput.get(value.session) ?? { data: [], closed: false }
        if (pending.data.length + (value.data?.length ?? 0) <= 65536) pending.data.push(...(value.data ?? []))
        pending.closed ||= !!value.closed
        this.terminalOutput.set(value.session, pending)
      }
      return
    }
    const value = event.type === 'EnvironmentStopped' ? event.value.state : event.value
    const entry = this.envs.get(value?.key)
    if (!entry) return
    if (event.type === 'EnvironmentProgress') {
      this.show(entry, { state: 'starting', message: STAGES[value.stage] ?? value.stage, stage: value.stage, fraction: value.fraction })
    } else if (event.type === 'EnvironmentState' || event.type === 'EnvironmentStopped') this.adopt(entry, value)
  }
  private adopt(entry: Env, value: any): EnvState {
    entry.control = value.controlDir
    switch (value.phase) {
      case EnvironmentPhase.Ready:
        return this.show(entry, { state: 'ready', endpoint: value.endpoint, token: value.token,
          projectId: value.project_id, unix: value.unixSocket, pid: value.pid, readyFile: value.readyFile,
          executionScopeUnavailable: value.executionScopeUnavailable })
      case EnvironmentPhase.Starting:
        return this.show(entry, { state: 'starting', message: this.vm ? L('実行環境を起動しています') : L('tsugumid を起動しています') })
      case EnvironmentPhase.Failed: {
        let diagnostic: HistoryVersionMismatch | undefined
        try { diagnostic = historyVersionMismatch(JSON.parse(value.startupErrorJson)) } catch {}
        return this.show(entry, { state: 'failed', message: value.message ?? '', recovery: value.recovery ?? '',
          log: value.log ?? '', historyVersionMismatch: diagnostic })
      }
      default: return this.show(entry, { state: 'stopped' })
    }
  }
  private async call(dir: string, action: string, archive = false): Promise<EnvState> {
    const entry = this.env(dir)
    const state = await this.request('EnvironmentRequest', { project: entry.project, action,
      origins: this.origins(), folders: entry.folders, archive_incompatible_history: archive })
    return this.adopt(entry, state)
  }
  state(dir: string) { return this.call(dir, 'State') }
  start(dir: string, archive = false) { return this.call(dir, 'Start', archive) }
  async stop(dir: string) { await this.call(dir, 'Stop') }
  stopAll() { return Promise.all([...this.envs.values()].map((entry) => this.stop(entry.project))) }
  async attachFolders(dir: string, _key: string, folders: string[]) {
    this.env(dir).folders = folders
    await this.call(dir, 'Folders')
  }
  controlDir(key: string) { return this.envs.get(key)?.control ?? path.join(this.vm?.support ?? this.support, this.vm ? 'environments' : 'env', key, 'control') }
  grantImport(dir: string, source: string) {
    const entry = this.env(dir)
    return writeGrant(this.controlDir(entry.key), source, (grant, dest) => this.vm ? `/control/grants/${grant}` : dest)
  }
  async terminal(dir: string, cols: number, rows: number): Promise<VMTerminal> {
    const ready = await this.start(dir)
    if (ready.state !== 'ready') throw new Error(ready.state === 'failed' ? ready.message : L('実行環境が起動していません'))
    const { session } = await this.request('HostTerminalRequest', { project: dir, action: 'Open', rows, cols })
    const handlers = { data: (_value: string) => {}, exit: (_value: { exitCode: number }) => {}, decoder: new TextDecoder() }
    this.terminals.set(session, handlers)
    const call = (action: string, fields: unknown = {}) => this.request('HostTerminalRequest', { action, session, ...fields as object }).catch(() => {})
    const terminal: VMTerminal = {
      cols, rows,
      write: (value) => void call('Input', { data: [...Buffer.from(value)] }),
      resize: (cols, rows) => { terminal.cols = cols; terminal.rows = rows; void call('Resize', { cols, rows }) },
      kill: () => void call('Close'),
      onData: (fn) => { handlers.data = fn; const pending = this.terminalOutput.get(session); if (pending?.data.length) { fn(handlers.decoder.decode(Uint8Array.from(pending.data), { stream: true })); pending.data = [] } },
      onExit: (fn) => { handlers.exit = fn; const pending = this.terminalOutput.get(session); if (pending?.closed) fn({ exitCode: 0 }); this.terminalOutput.delete(session) },
    }
    return terminal
  }
  list() { return [...this.envs.values()].map(({ key, project, state }) => ({ key, project, state })) }
}
