import { modelErrorMessage } from './i18n'
import { pageRoutes, RouteError } from './host/routes'
// Calls to hostd (files, processes, the Agent), assembled in the page.

/** The page's host routes (src/host/routes.ts): Host and Agent RPC to hostd. */
let routes: ReturnType<typeof pageRoutes> | undefined
const local = () => (routes ??= pageRoutes((v) => {
  emit(v)
  if ((v as { type?: string })?.type === 'guideChanged') broadcastPages(v)
}))

/** What answers api() and feeds onServerEvent: the page's routes, or a
 *  test double (tests/fetchTransport.ts). */
export interface ApiTransport {
  call(path: string, body: unknown, signal?: AbortSignal): Promise<unknown>
  start(emit: (v: unknown) => void): void
}
let transport: ApiTransport | undefined
export function useApiTransport(t: ApiTransport) { transport = t }

export async function api<T = any>(path: string, body?: unknown, signal?: AbortSignal): Promise<T> {
  let v: any
  try {
    v = transport ? await transport.call(path, body, signal) : await local().call(body === undefined ? 'GET' : 'POST', path, body, signal)
  } catch (e) {
    if (e instanceof RouteError || transport && e instanceof Error) throw new Error(modelErrorMessage(e.message, (e as { code?: string }).code))
    throw e
  }
  if (path === '/api/settings' && body !== undefined) emit({ type: 'settingsChanged', provider: v.chatProvider })
  return v as T
}

type Listener = (e: any) => void
const listeners = new Set<Listener>()
function emit(v: unknown) {
  for (const l of [...listeners]) l(v)
}
let started = false
let reported: string | undefined
const detached = () => new URLSearchParams(globalThis.location?.search).has('scope')
const report = (project?: string) => (transport
  ? transport.call('/api/page', { project, detached: detached() })
  : local().core.user.call('HostPageRequest', { project, detached: detached() })).catch(() => {})

/** Tells hostd what this page shows now (it picks a free page for Claude Code's projects). */
export function reportPageProject(project?: string) {
  reported = project
  void report(project)
}

/** This page left a project. When no other page shows it, hostd tells the
 *  pages (a child workflow's page closes with it). */
export async function closePageProject(_project: string) {
  reported = undefined
  await report(undefined)
  return { ok: true }
}

/** Claude Code waits for the person's decision, now or since before this page
 *  opened: the page on its project shows it. */
async function showExternalDecisions() {
  try {
    const list = await local().core.user.call<{ decisions: { id: string; state: string; conversation_id: string; project_path?: string; asked: string[]; call_id?: string }[] }>('DecisionListRequest', { pending_only: true })
    for (const r of list.decisions)
      if (r.state === 'pending' && r.conversation_id.startsWith('external-') && r.project_path)
        emit({ type: 'chat', project: r.project_path, session: 'external',
          event: { kind: 'decision', id: r.id, asked: r.asked, call_id: r.call_id, from: 'Claude Code' } })
  } catch { /* the next change reads it again */ }
}

/** The guide's progress is the same on every page of this origin. */
let guideChannel: BroadcastChannel | undefined

/** Events of hostd and this page: page routing, settings, the guide, Claude Code's decisions. */
export function onServerEvent(fn: Listener): () => void {
  listeners.add(fn)
  if (!started && transport) {
    started = true
    transport.start(emit)
  }
  if (!started) {
    started = true
    const core = local().core
    let lost = false
    core.observeHost(({ type, value }) => {
      if (type === 'PageShow') emit({ type: 'agentOpen', project: value.project })
      else if (type === 'PageClosed') emit({ type: 'projectClosed', project: value.project })
      else if (type === 'ConversationChanged' && String(value.conversation_id ?? '').startsWith('external-')) void showExternalDecisions()
      else if (type === 'Disconnected') {
        lost = true
        // Events sent while the socket was down are lost: once it is back,
        // every listener reads its state again ({ type: 'reconnected' }).
        const retry = () => void core.connectHost().catch((error) => {
          emit({ type: 'hostUnavailable', message: (error as Error).message })
          setTimeout(retry, 1000)
        })
        setTimeout(retry, 200)
      } else if (type === 'Connected' && lost) {
        lost = false
        void report(reported)
      }
    })
    if (typeof BroadcastChannel !== 'undefined') {
      guideChannel = new BroadcastChannel('tsugumi.guide')
      guideChannel.onmessage = (m) => emit(m.data)
    }
    void core.connectHost().then(() => { void showExternalDecisions(); return report(reported) }).catch(() => {})
  }
  return () => listeners.delete(fn)
}

/** Sends an event to the other pages of this origin too (the guide). */
export function broadcastPages(v: unknown) {
  guideChannel?.postMessage(v)
}

export interface SetupStatus {
  tsugumid: { path: string; ok: boolean }
  worker: { path: string; ok: boolean }
  nix: { ok: boolean }
  repo: string
}

export interface Demo {
  id: string
  title: string
  description: string
  path: string
  tags: string[]
  /** The field the guide lists it under (an implemented demo; none: a Draft). */
  field?: string
}

export interface Status {
  setup: SetupStatus
  demos: Demo[]
  recents: { path: string; name: string; openedAt: number }[]
  home: string
  /** Where demo copies go (~/Documents/Tsugumi or TSUGUMI_PROJECTS_DIR). */
  documents: string
  launchProject: string | null
  /** Cards can use folders of this host at /host/<name>. */
  hostFolders: boolean
  /** The nixpkgs this Tsugumi builds with (new projects are pinned to it). */
  agentExclude: string[]
  chatAvailable: boolean
  chatError: string | null
}

export interface FolderListing {
  path: string
  parent: string | null
  project: boolean
  entries: { name: string; project: boolean }[]
}

/** The folder a new project's picker starts in: the projects folder (Status.documents),
 * made with its missing parents; home when it cannot be made. */
export async function projectsFolder(status: Pick<Status, 'documents' | 'home'>): Promise<string> {
  const exists = (path: string) => api<FolderListing>(`/api/fs?path=${encodeURIComponent(path)}`).then(() => true, () => false)
  const missing: string[] = []
  let parent = status.documents.replace(/\/+$/, '')
  while (parent.lastIndexOf('/') > 0 && !await exists(parent)) {
    missing.unshift(parent.slice(parent.lastIndexOf('/') + 1))
    parent = parent.slice(0, parent.lastIndexOf('/'))
  }
  try {
    for (const name of missing) {
      await api<FolderListing>('/api/fs/mkdir', { parent, name })
      parent = `${parent}/${name}`
    }
    return status.documents
  } catch {
    return status.home
  }
}
