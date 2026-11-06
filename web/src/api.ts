// Calls to the host server (files, processes, the Agent). Mutating calls
// carry a header that a cross-origin page cannot send.

export class ApiError extends Error {}

export async function api<T = any>(path: string, body?: unknown): Promise<T> {
  const r = await fetch(path, body === undefined
    ? { headers: { 'x-tsugumi': '1' } }
    : { method: 'POST', headers: { 'content-type': 'application/json', 'x-tsugumi': '1' }, body: JSON.stringify(body) })
  const v = await r.json().catch(() => ({ error: `HTTP ${r.status}` }))
  if (!r.ok) throw new ApiError(v.error ?? `HTTP ${r.status}`)
  return v as T
}

type Listener = (e: any) => void
const listeners = new Set<Listener>()
let source: EventSource | undefined

/** Events from the host server: environment states, terminals and Agent progress. */
export function onServerEvent(fn: Listener): () => void {
  listeners.add(fn)
  if (!source) {
    source = new EventSource('/api/events')
    // Events sent while the stream was down are lost: after it comes back,
    // every listener reads its state again ({ type: 'reconnected' }).
    let lost = false
    source.onerror = () => {
      lost = true
    }
    source.onopen = () => {
      if (!lost) return
      lost = false
      for (const l of [...listeners]) l({ type: 'reconnected' })
    }
    source.onmessage = (m) => {
      let v: unknown
      try {
        v = JSON.parse(m.data)
      } catch {
        return
      }
      for (const l of [...listeners]) l(v)
    }
  }
  return () => listeners.delete(fn)
}

export interface EnvPublicState {
  state: 'stopped' | 'starting' | 'ready' | 'failed'
  message?: string
  recovery?: string
  log?: string
  endpoint?: string
  token?: string
  projectId?: string
  readyFile?: string
}

export interface SetupStatus {
  tsugumid: { path: string; ok: boolean }
  worker: { path: string; ok: boolean }
  sdk: { path: string; ok: boolean }
  nix: { ok: boolean }
  repo: string
  building: { running: boolean; code?: number; log: string } | null
}

export interface Demo {
  id: string
  title: string
  description: string
  path: string
  tags: string[]
}

export interface Status {
  setup: SetupStatus
  demos: Demo[]
  recents: { path: string; name: string; openedAt: number }[]
  home: string
  /** Where demo copies go (~/Documents/Tsugumi or TSUGUMI_PROJECTS_DIR). */
  documents: string
  launchProject: string | null
  /** The nixpkgs this Tsugumi builds with (new projects are pinned to it). */
  nixpkgsPin: string | null
  chatAvailable: boolean
  chatError: string | null
}

export interface FolderListing {
  path: string
  parent: string | null
  project: boolean
  entries: { name: string; project: boolean }[]
}

/** Events of a terminal (a shell in the project's folder on this host). */
export type TerminalEvent =
  | { type: 'terminal'; id: string; data: string }
  | { type: 'terminal-exit'; id: string; code: number | null; signal: string | null }
