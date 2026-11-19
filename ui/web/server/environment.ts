// Presentation adapter for core-owned project environments.
import fs from 'node:fs'
import path from 'node:path'
import { TextDecoder } from 'node:util'
import { L } from './i18n'
import { HistoryVersionMismatch, historyVersionMismatch } from '../shared/historyVersion'
import { EnvironmentPhase } from '../shared/gen/tsugumi/host/environment-phase'

const STAGES: Record<string, string> = {
  'installing the VM image': L('VM イメージを展開しています'),
  'creating the Nix store disk': L('Nix store の保存領域を作っています'),
  'booting the VM': L('VM を起動しています'),
  'mounting the Nix store disk': L('Nix store の保存領域を開いています（初回は初期化）'),
  'checking the Nix store after an unclean stop': L('前回の VM が正常に停止しなかったため、Nix store を検査・修復しています'),
  'checking the Nix store contents': L('Nix store の内容を検査・修復しています（初回と VM イメージの更新後）'),
  'starting the Nix daemon': L('Nix を準備しています'),
  'configuring the network': L('ネットワークを設定しています'),
  'starting the supervisor': L('実行環境の管理プロセスを起動しています'),
  'starting tsugumid': L('このプロジェクトの tsugumid を起動しています'),
  'waiting for tsugumid to become ready': L('tsugumid の応答を待っています'),
}

export interface VMHostOptions { host: string; image: string; support: string }
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
  readonly hostArguments?: string[]
  state(dir: string): Promise<EnvState>
  start(dir: string): Promise<EnvState>
  stop(dir: string): Promise<void>
  /** The folders a project's chats may read now. */
  attachFolders(dir: string, folders: string[]): Promise<{ folder: string; resource: string }[]>
  removeFolders(dir: string, folders: string[]): Promise<void>
  list(): { key: string; project: string; state: EnvState }[]
}

/** The host owns execution identity; the relay indexes its views by folder. */
export function projectKey(dir: string): { key: string; real: string } {
  const real = fs.realpathSync(dir)
  return { key: real, real }
}

interface Env { hostKey?: string; key: string; project: string; state: EnvState }

export class EnvironmentManager implements Environments {
  private envs = new Map<string, Env>()
  private listeners = new Set<(key: string, state: EnvState) => void>()
  private observing = false
  private terminals = new Map<number, { data: (value: string) => void; exit: (value: { exitCode: number }) => void; decoder: TextDecoder }>()
  private terminalOutput = new Map<number, { data: number[]; closed: boolean }>()
  private openingTerminals = 0
  readonly hostArguments: string[]

  constructor(readonly bins: Binaries, readonly support: string, private origins: () => string[],
    private request: Request, private observeHost: Observe, readonly vm?: VMHostOptions) {
    this.hostArguments = vm
      ? ['--vmhost', vm.host, '--vm-support', vm.support, '--vm-image', vm.image]
      : ['--tsugumid', bins.tsugumid, '--worker', bins.worker, '--sdk-dir', bins.sdkDir,
         '--environment-dir', path.join(support, 'env')]
  }
  private env(dir: string): Env {
    const { key, real } = projectKey(dir)
    let entry = this.envs.get(key)
    if (!entry) this.envs.set(key, entry = { key, project: real, state: { state: 'stopped' } })
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
      } else if (this.openingTerminals > 0) {
        const pending = this.terminalOutput.get(value.session) ?? { data: [], closed: false }
        const maxBuffered = 1024 * 1024
        if (value.data?.length && pending.data.length < maxBuffered) {
          const room = maxBuffered - pending.data.length
          pending.data.push(...value.data.slice(0, room))
        }
        pending.closed ||= !!value.closed
        this.terminalOutput.set(value.session, pending)
      }
      return
    }
    const value = event.type === 'EnvironmentStopped' ? event.value.state : event.value
    const entry = this.envs.get(value?.project) ?? [...this.envs.values()].find(e => e.hostKey === value?.key)
    if (!entry) return
    if (event.type === 'EnvironmentProgress') {
      this.show(entry, { state: 'starting', message: STAGES[value.stage] ?? value.stage, stage: value.stage, fraction: value.fraction })
    } else if (event.type === 'EnvironmentState' || event.type === 'EnvironmentStopped') this.adopt(entry, value)
  }
  private adopt(entry: Env, value: any): EnvState {
    entry.hostKey = value.key
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
  private async call(dir: string, action: string): Promise<EnvState> {
    const entry = this.env(dir)
    const state = await this.request('EnvironmentRequest', { project: entry.project, action,
      origins: this.origins() })
    return this.adopt(entry, state)
  }
  state(dir: string) { return this.call(dir, 'State') }
  start(dir: string) { return this.call(dir, 'Start') }
  async stop(dir: string) { await this.call(dir, 'Stop') }
  async attachFolders(dir: string, folders: string[]) {
    const entry = this.env(dir)
    const reply = await this.request('AttachFoldersRequest', { project: entry.project, folders })
    this.adopt(entry, reply.state)
    return reply.folders as { folder: string; resource: string }[]
  }
  async removeFolders(dir: string, folders: string[]) {
    await this.request('EnvironmentRequest', { project: this.env(dir).project, action: 'RemoveFolders', folders })
  }
  async terminal(dir: string, cols: number, rows: number): Promise<VMTerminal> {
    const ready = await this.start(dir)
    if (ready.state !== 'ready') throw new Error(ready.state === 'failed' ? ready.message : L('実行環境が起動していません'))
    this.openingTerminals++
    let session: number
    try {
      const reply = await this.request('HostTerminalRequest', { project: dir, action: 'Open', rows, cols })
      session = reply.session
    } finally {
      this.openingTerminals--
    }
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
